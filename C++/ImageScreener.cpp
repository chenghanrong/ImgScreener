// ============================================================
// ImageScreenerApp.cpp
//
// VS2017 / Win32 / WIC
//
// 最终完整优化版
//
// ============================================================
//
// 功能：
//
// 1. 固定扫描线程
// 2. 固定图片解码线程
// 3. 固定原图像素查询线程
// 4. 当前图片 + 下一张预加载
// 5. 上一张按需解码
// 6. 当前图片解码门控
// 7. WIC Linear 缩放
// 8. 预览图只保存显示尺寸
// 9. 鼠标立即读取显示 Bitmap 像素
// 10. 后台 WIC 查询原图真实像素
// 11. 彩色图支持 R/G/B/Gray/HEX
// 12. 灰度图支持 Gray
// 13. 显示坐标 + 原图坐标
// 14. 像素查询限频约 30 FPS
// 15. Pixel ToolTip
// 16. 像素栏单独一行
// 17. 像素信息自动省略
// 18. 递归扫描
// 19. 保存相对路径
// 20. 保存目录自动排除
// 21. saved_list.txt UTF-8 BOM
// 22. saved_list.txt 优先恢复
// 23. 解码失败自动跳过
// 24. 连续失败自动暂停
// 25. 关闭时统一停止线程
// 26. VS2017 兼容
//
// ============================================================

#define NOMINMAX

#include <windows.h>
#include <windowsx.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <wincodec.h>
#include <commctrl.h>
#include <shlobj.h>

#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <algorithm>
#include <unordered_set>
#include <cwctype>
#include <cstdio>
#include <wchar.h>
#include <exception>
#include <cstdint>
#include <cmath>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

// ============================================================
// ID
// ============================================================

#define ID_BTN_OPEN         1001
#define ID_BTN_SAVE_DIR     1002
#define ID_BTN_PREV         1003
#define ID_BTN_PAUSE        1004
#define ID_BTN_NEXT         1005
#define ID_BTN_SAVE         1006

#define ID_SPEED            1007
#define ID_SPEED_UP         1008
#define ID_SPEED_DOWN       1009

#define ID_CHECK_RECURSIVE  1010
#define ID_CHECK_AUTOSKIP   1011

#define ID_LABEL_INFO       1101
#define ID_LABEL_SAVED      1102
#define ID_LABEL_PROGRESS   1103
#define ID_LABEL_SAVE_DIR   1104
#define ID_LABEL_PIXEL      1105

#define TIMER_PLAY          2001
#define TIMER_RESIZE        2002

#define WM_APP_SCAN_DONE    (WM_APP + 1)
#define WM_APP_DECODE_DONE  (WM_APP + 2)
#define WM_APP_PIXEL_DONE   (WM_APP + 3)

// ============================================================
// Clamp
// ============================================================

template <typename T>
T ClampValue(
	T value,
	T minValue,
	T maxValue)
{
	if (value < minValue)
		return minValue;

	if (value > maxValue)
		return maxValue;

	return value;
}

// ============================================================
// Bitmap
// ============================================================

struct BitmapData
{
	HBITMAP hBitmap;

	int width;
	int height;

	void* bits;

	int sourceWidth;
	int sourceHeight;

	BitmapData()
		: hBitmap(nullptr)
		, width(0)
		, height(0)
		, bits(nullptr)
		, sourceWidth(0)
		, sourceHeight(0)
	{
	}
};

static void DeleteBitmapData(
	BitmapData& b)
{
	if (b.hBitmap)
	{
		DeleteObject(b.hBitmap);
		b.hBitmap = nullptr;
	}

	b.width = 0;
	b.height = 0;
	b.bits = nullptr;

	b.sourceWidth = 0;
	b.sourceHeight = 0;
}

static void MoveBitmapData(
	BitmapData& dst,
	BitmapData& src)
{
	DeleteBitmapData(dst);

	dst.hBitmap = src.hBitmap;
	dst.width = src.width;
	dst.height = src.height;
	dst.bits = src.bits;

	dst.sourceWidth = src.sourceWidth;
	dst.sourceHeight = src.sourceHeight;

	src.hBitmap = nullptr;
	src.width = 0;
	src.height = 0;
	src.bits = nullptr;

	src.sourceWidth = 0;
	src.sourceHeight = 0;
}

// ============================================================
// File
// ============================================================

static std::wstring GetFileNameOnly(
	const std::wstring& path)
{
	if (path.empty())
		return L"";

	const size_t pos =
		path.find_last_of(L"\\/");

	if (pos == std::wstring::npos)
		return path;

	if (pos + 1 >= path.size())
		return L"";

	return path.substr(pos + 1);
}

static std::wstring GetFileExtension(
	const std::wstring& path)
{
	std::wstring fileName =
		GetFileNameOnly(path);

	const size_t pos =
		fileName.find_last_of(L'.');

	if (pos == std::wstring::npos)
		return L"";

	std::wstring ext =
		fileName.substr(pos);

	for (size_t i = 0; i < ext.size(); ++i)
	{
		ext[i] =
			static_cast<wchar_t>(
				towlower(ext[i]));
	}

	return ext;
}

static bool IsSupportedImage(
	const std::wstring& path)
{
	const std::wstring ext =
		GetFileExtension(path);

	return
		ext == L".jpg" ||
		ext == L".jpeg" ||
		ext == L".png" ||
		ext == L".bmp" ||
		ext == L".gif" ||
		ext == L".tif" ||
		ext == L".tiff";
}

// ============================================================
// UTF-8
// ============================================================

static std::string WideToUtf8(
	const std::wstring& text)
{
	if (text.empty())
		return std::string();

	int bytes =
		WideCharToMultiByte(
			CP_UTF8,
			0,
			text.c_str(),
			static_cast<int>(text.size()),
			nullptr,
			0,
			nullptr,
			nullptr);

	if (bytes <= 0)
		return std::string();

	std::string result(
		static_cast<size_t>(bytes),
		'\0');

	WideCharToMultiByte(
		CP_UTF8,
		0,
		text.c_str(),
		static_cast<int>(text.size()),
		&result[0],
		bytes,
		nullptr,
		nullptr);

	return result;
}

static std::wstring Utf8ToWide(
	const std::string& text)
{
	if (text.empty())
		return L"";

	int chars =
		MultiByteToWideChar(
			CP_UTF8,
			0,
			text.data(),
			static_cast<int>(text.size()),
			nullptr,
			0);

	if (chars <= 0)
		return L"";

	std::wstring result(
		static_cast<size_t>(chars),
		L'\0');

	MultiByteToWideChar(
		CP_UTF8,
		0,
		text.data(),
		static_cast<int>(text.size()),
		&result[0],
		chars);

	return result;
}

// ============================================================
// Path
// ============================================================

static std::wstring NormalizeFolderPathForCompare(
	const std::wstring& path)
{
	if (path.empty())
		return L"";

	wchar_t full[MAX_PATH * 8] = { 0 };

	DWORD len =
		GetFullPathNameW(
			path.c_str(),
			_countof(full),
			full,
			nullptr);

	std::wstring result =
		(len > 0 && len < _countof(full))
		? std::wstring(full, len)
		: path;

	while (
		result.size() > 1 &&
		(result.back() == L'\\' ||
			result.back() == L'/'))
	{
		result.pop_back();
	}

	for (size_t i = 0; i < result.size(); ++i)
	{
		result[i] =
			static_cast<wchar_t>(
				towlower(result[i]));
	}

	return result;
}

static bool SameFolderPath(
	const std::wstring& a,
	const std::wstring& b)
{
	if (a.empty() || b.empty())
		return false;

	return
		NormalizeFolderPathForCompare(a) ==
		NormalizeFolderPathForCompare(b);
}

static bool IsPathInsideFolder(
	const std::wstring& childPath,
	const std::wstring& parentPath)
{
	if (childPath.empty() ||
		parentPath.empty())
	{
		return false;
	}

	std::wstring child =
		NormalizeFolderPathForCompare(
			childPath);

	std::wstring parent =
		NormalizeFolderPathForCompare(
			parentPath);

	if (child.empty() ||
		parent.empty())
	{
		return false;
	}

	if (child == parent)
		return true;

	if (parent.back() != L'\\')
		parent += L'\\';

	if (child.size() <= parent.size())
		return false;

	return
		child.compare(
			0,
			parent.size(),
			parent) == 0;
}

static std::wstring GetRelativePath(
	const std::wstring& root,
	const std::wstring& full)
{
	if (root.empty() || full.empty())
		return L"";

	std::wstring rootNorm =
		NormalizeFolderPathForCompare(root);

	std::wstring fullNorm =
		NormalizeFolderPathForCompare(full);

	if (fullNorm == rootNorm)
		return L"";

	if (rootNorm.back() != L'\\')
		rootNorm += L'\\';

	if (
		fullNorm.compare(
			0,
			rootNorm.size(),
			rootNorm) != 0)
	{
		return L"";
	}

	std::wstring relative =
		full.substr(root.size());

	while (
		!relative.empty() &&
		(relative[0] == L'\\' ||
			relative[0] == L'/'))
	{
		relative.erase(
			relative.begin());
	}

	return relative;
}

static std::wstring NormalizeRelativePath(
	const std::wstring& path)
{
	std::wstring result = path;

	for (size_t i = 0;
		i < result.size();
		++i)
	{
		if (result[i] == L'/')
			result[i] = L'\\';

		result[i] =
			static_cast<wchar_t>(
				towlower(result[i]));
	}

	while (
		!result.empty() &&
		result.front() == L'\\')
	{
		result.erase(
			result.begin());
	}

	return result;
}

// ============================================================
// Directory
// ============================================================

static bool CreateDirectoryRecursive(
	const std::wstring& path)
{
	if (path.empty())
		return false;

	DWORD attr =
		GetFileAttributesW(
			path.c_str());

	if (attr != INVALID_FILE_ATTRIBUTES)
	{
		return
			(attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
	}

	int ret =
		SHCreateDirectoryExW(
			nullptr,
			path.c_str(),
			nullptr);

	if (
		ret == ERROR_SUCCESS ||
		ret == ERROR_ALREADY_EXISTS ||
		ret == ERROR_FILE_EXISTS)
	{
		return true;
	}

	DWORD attr2 =
		GetFileAttributesW(
			path.c_str());

	return
		attr2 != INVALID_FILE_ATTRIBUTES &&
		(attr2 & FILE_ATTRIBUTE_DIRECTORY);
}

// ============================================================
// Scan
// ============================================================

static std::vector<std::wstring> ScanImageFiles(
	const std::wstring& folder,
	bool recursive,
	const std::vector<std::wstring>& excludeFolders)
{
	std::vector<std::wstring> files;

	if (folder.empty())
		return files;

	std::vector<std::wstring> pending;
	pending.push_back(folder);

	while (!pending.empty())
	{
		std::wstring current =
			pending.back();

		pending.pop_back();

		bool excluded = false;

		for (size_t i = 0;
			i < excludeFolders.size();
			++i)
		{
			if (
				SameFolderPath(
					current,
					excludeFolders[i]))
			{
				excluded = true;
				break;
			}
		}

		if (excluded)
			continue;

		std::wstring searchPath =
			current;

		if (
			!searchPath.empty() &&
			searchPath.back() != L'\\' &&
			searchPath.back() != L'/')
		{
			searchPath += L'\\';
		}

		searchPath += L"*.*";

		WIN32_FIND_DATAW data;

		ZeroMemory(
			&data,
			sizeof(data));

		HANDLE hFind =
			FindFirstFileW(
				searchPath.c_str(),
				&data);

		if (hFind == INVALID_HANDLE_VALUE)
			continue;

		do
		{
			const std::wstring name =
				data.cFileName;

			if (
				name == L"." ||
				name == L"..")
			{
				continue;
			}

			std::wstring full =
				current;

			if (
				!full.empty() &&
				full.back() != L'\\' &&
				full.back() != L'/')
			{
				full += L'\\';
			}

			full += name;

			if (
				data.dwFileAttributes &
				FILE_ATTRIBUTE_DIRECTORY)
			{
				if (
					recursive &&
					!(data.dwFileAttributes &
						FILE_ATTRIBUTE_REPARSE_POINT))
				{
					bool childExcluded = false;

					for (
						size_t i = 0;
						i < excludeFolders.size();
						++i)
					{
						if (
							SameFolderPath(
								full,
								excludeFolders[i]))
						{
							childExcluded = true;
							break;
						}
					}

					if (!childExcluded)
					{
						pending.push_back(
							full);
					}
				}

				continue;
			}

			if (
				data.dwFileAttributes &
				FILE_ATTRIBUTE_REPARSE_POINT)
			{
				continue;
			}

			if (!IsSupportedImage(name))
				continue;

			files.push_back(full);

		} while (
			FindNextFileW(
				hFind,
				&data));

		FindClose(hFind);
	}

	std::sort(
		files.begin(),
		files.end(),
		[](const std::wstring& a,
			const std::wstring& b)
	{
		return
			_wcsicmp(
				a.c_str(),
				b.c_str()) < 0;
	});

	return files;
}

// ============================================================
// Preview decoder
// ============================================================

class ImageDecoder
{
public:

	ImageDecoder()
		: factory_(nullptr)
		, comInitialized_(false)
	{
		HRESULT hr =
			CoInitializeEx(
				nullptr,
				COINIT_MULTITHREADED);

		if (SUCCEEDED(hr))
		{
			comInitialized_ = true;
		}
		else if (
			hr == RPC_E_CHANGED_MODE)
		{
			comInitialized_ = false;
		}
		else
		{
			return;
		}

		hr =
			CoCreateInstance(
				CLSID_WICImagingFactory,
				nullptr,
				CLSCTX_INPROC_SERVER,
				IID_PPV_ARGS(
					&factory_));

		if (FAILED(hr))
			factory_ = nullptr;
	}

	~ImageDecoder()
	{
		if (factory_)
		{
			factory_->Release();
			factory_ = nullptr;
		}

		if (comInitialized_)
			CoUninitialize();
	}

	bool IsReady() const
	{
		return factory_ != nullptr;
	}

	bool DecodeFit(
		const std::wstring& file,
		UINT maxW,
		UINT maxH,
		BitmapData& output)
	{
		DeleteBitmapData(output);

		if (
			!factory_ ||
			file.empty() ||
			maxW == 0 ||
			maxH == 0)
		{
			return false;
		}

		IWICBitmapDecoder* decoder = nullptr;
		IWICBitmapFrameDecode* frame = nullptr;
		IWICBitmapScaler* scaler = nullptr;
		IWICFormatConverter* converter = nullptr;

		HBITMAP bitmap = nullptr;

		bool success = false;

		do
		{
			HRESULT hr =
				factory_->CreateDecoderFromFilename(
					file.c_str(),
					nullptr,
					GENERIC_READ,
					WICDecodeMetadataCacheOnLoad,
					&decoder);

			if (FAILED(hr))
				break;

			hr =
				decoder->GetFrame(
					0,
					&frame);

			if (FAILED(hr))
				break;

			UINT srcW = 0;
			UINT srcH = 0;

			hr =
				frame->GetSize(
					&srcW,
					&srcH);

			if (
				FAILED(hr) ||
				srcW == 0 ||
				srcH == 0)
			{
				break;
			}

			double scaleW =
				static_cast<double>(maxW) /
				static_cast<double>(srcW);

			double scaleH =
				static_cast<double>(maxH) /
				static_cast<double>(srcH);

			double scale =
				(std::min)(
					1.0,
					(std::min)(
						scaleW,
						scaleH));

			UINT dstW =
				static_cast<UINT>(
					srcW * scale);

			UINT dstH =
				static_cast<UINT>(
					srcH * scale);

			if (dstW < 1)
				dstW = 1;

			if (dstH < 1)
				dstH = 1;

			IWICBitmapSource* source =
				frame;

			if (
				dstW != srcW ||
				dstH != srcH)
			{
				hr =
					factory_->CreateBitmapScaler(
						&scaler);

				if (FAILED(hr))
					break;

				hr =
					scaler->Initialize(
						frame,
						dstW,
						dstH,
						WICBitmapInterpolationModeLinear);

				if (FAILED(hr))
					break;

				source =
					scaler;
			}

			hr =
				factory_->CreateFormatConverter(
					&converter);

			if (FAILED(hr))
				break;

			hr =
				converter->Initialize(
					source,
					GUID_WICPixelFormat32bppBGRA,
					WICBitmapDitherTypeNone,
					nullptr,
					0.0,
					WICBitmapPaletteTypeCustom);

			if (FAILED(hr))
				break;

			BITMAPINFO bi;

			ZeroMemory(
				&bi,
				sizeof(bi));

			bi.bmiHeader.biSize =
				sizeof(BITMAPINFOHEADER);

			bi.bmiHeader.biWidth =
				static_cast<LONG>(dstW);

			bi.bmiHeader.biHeight =
				-static_cast<LONG>(dstH);

			bi.bmiHeader.biPlanes = 1;
			bi.bmiHeader.biBitCount = 32;
			bi.bmiHeader.biCompression = BI_RGB;

			void* bits = nullptr;

			HDC hdc =
				GetDC(nullptr);

			bitmap =
				CreateDIBSection(
					hdc,
					&bi,
					DIB_RGB_COLORS,
					&bits,
					nullptr,
					0);

			ReleaseDC(
				nullptr,
				hdc);

			if (!bitmap || !bits)
				break;

			UINT stride =
				dstW * 4U;

			UINT bufferSize =
				stride * dstH;

			hr =
				converter->CopyPixels(
					nullptr,
					stride,
					bufferSize,
					static_cast<BYTE*>(bits));

			if (FAILED(hr))
				break;

			output.hBitmap = bitmap;
			output.width =
				static_cast<int>(dstW);
			output.height =
				static_cast<int>(dstH);
			output.bits =
				bits;
			output.sourceWidth =
				static_cast<int>(srcW);
			output.sourceHeight =
				static_cast<int>(srcH);

			bitmap = nullptr;

			success = true;

		} while (false);

		if (bitmap)
			DeleteObject(bitmap);

		if (converter)
			converter->Release();

		if (scaler)
			scaler->Release();

		if (frame)
			frame->Release();

		if (decoder)
			decoder->Release();

		return success;
	}

private:

	IWICImagingFactory* factory_;
	bool comInitialized_;
};

// ============================================================
// Pixel Result
// ============================================================

struct PixelResult
{
	bool valid;

	unsigned long requestId;

	int sourceX;
	int sourceY;

	int r;
	int g;
	int b;
	int a;

	int gray;

	std::wstring path;

	PixelResult()
		: valid(false)
		, requestId(0)
		, sourceX(0)
		, sourceY(0)
		, r(0)
		, g(0)
		, b(0)
		, a(255)
		, gray(0)
	{
	}
};

// ============================================================
// 原图像素读取
// ============================================================

class PixelDecoder
{
public:

	PixelDecoder()
		: factory_(nullptr)
		, decoder_(nullptr)
		, frame_(nullptr)
		, converter_(nullptr)
		, comInitialized_(false)
		, sourceWidth_(0)
		, sourceHeight_(0)
	{
		HRESULT hr =
			CoInitializeEx(
				nullptr,
				COINIT_MULTITHREADED);

		if (SUCCEEDED(hr))
		{
			comInitialized_ = true;
		}
		else if (
			hr == RPC_E_CHANGED_MODE)
		{
			comInitialized_ = false;
		}
		else
		{
			return;
		}

		hr =
			CoCreateInstance(
				CLSID_WICImagingFactory,
				nullptr,
				CLSCTX_INPROC_SERVER,
				IID_PPV_ARGS(
					&factory_));

		if (FAILED(hr))
			factory_ = nullptr;
	}

	~PixelDecoder()
	{
		ReleaseSource();

		if (factory_)
		{
			factory_->Release();
			factory_ = nullptr;
		}

		if (comInitialized_)
			CoUninitialize();
	}

	bool IsReady() const
	{
		return factory_ != nullptr;
	}

	void ReleaseSource()
	{
		if (converter_)
		{
			converter_->Release();
			converter_ = nullptr;
		}

		if (frame_)
		{
			frame_->Release();
			frame_ = nullptr;
		}

		if (decoder_)
		{
			decoder_->Release();
			decoder_ = nullptr;
		}

		currentPath_.clear();

		sourceWidth_ = 0;
		sourceHeight_ = 0;
	}

	bool EnsureSource(
		const std::wstring& path)
	{
		if (
			!factory_ ||
			path.empty())
		{
			return false;
		}

		if (
			path == currentPath_ &&
			converter_ != nullptr)
		{
			return true;
		}

		ReleaseSource();

		HRESULT hr =
			factory_->CreateDecoderFromFilename(
				path.c_str(),
				nullptr,
				GENERIC_READ,
				WICDecodeMetadataCacheOnLoad,
				&decoder_);

		if (FAILED(hr))
			return false;

		hr =
			decoder_->GetFrame(
				0,
				&frame_);

		if (FAILED(hr))
		{
			ReleaseSource();
			return false;
		}

		UINT width = 0;
		UINT height = 0;

		hr =
			frame_->GetSize(
				&width,
				&height);

		if (
			FAILED(hr) ||
			width == 0 ||
			height == 0)
		{
			ReleaseSource();
			return false;
		}

		hr =
			factory_->CreateFormatConverter(
				&converter_);

		if (FAILED(hr))
		{
			ReleaseSource();
			return false;
		}

		hr =
			converter_->Initialize(
				frame_,
				GUID_WICPixelFormat32bppBGRA,
				WICBitmapDitherTypeNone,
				nullptr,
				0.0,
				WICBitmapPaletteTypeCustom);

		if (FAILED(hr))
		{
			ReleaseSource();
			return false;
		}

		currentPath_ =
			path;

		sourceWidth_ =
			static_cast<int>(width);

		sourceHeight_ =
			static_cast<int>(height);

		return true;
	}

	bool GetPixel(
		const std::wstring& path,
		int x,
		int y,
		PixelResult& result)
	{
		result.valid = false;

		if (
			!EnsureSource(path) ||
			!converter_)
		{
			return false;
		}

		if (
			x < 0 ||
			y < 0 ||
			x >= sourceWidth_ ||
			y >= sourceHeight_)
		{
			return false;
		}

		WICRect rect;

		rect.X = x;
		rect.Y = y;
		rect.Width = 1;
		rect.Height = 1;

		BYTE pixel[4] =
		{
			0,
			0,
			0,
			255
		};

		HRESULT hr =
			converter_->CopyPixels(
				&rect,
				4,
				4,
				pixel);

		if (FAILED(hr))
			return false;

		const int b =
			static_cast<int>(
				pixel[0]);

		const int g =
			static_cast<int>(
				pixel[1]);

		const int r =
			static_cast<int>(
				pixel[2]);

		const int a =
			static_cast<int>(
				pixel[3]);

		const int gray =
			static_cast<int>(
				0.299 * r +
				0.587 * g +
				0.114 * b +
				0.5);

		result.valid = true;

		result.sourceX = x;
		result.sourceY = y;

		result.r = r;
		result.g = g;
		result.b = b;
		result.a = a;
		result.gray = gray;

		result.path =
			path;

		return true;
	}

private:

	IWICImagingFactory* factory_;

	IWICBitmapDecoder* decoder_;
	IWICBitmapFrameDecode* frame_;
	IWICFormatConverter* converter_;

	bool comInitialized_;

	std::wstring currentPath_;

	int sourceWidth_;
	int sourceHeight_;
};

// ============================================================
// App
// ============================================================

class ImageScreenerApp
{
private:

	enum class DecodeTaskKind
	{
		Current,
		PreloadNext
	};

	struct DecodeTask
	{
		DecodeTaskKind kind;

		unsigned long requestId;

		std::wstring path;

		UINT width;
		UINT height;

		DecodeTask()
			: kind(DecodeTaskKind::Current)
			, requestId(0)
			, width(0)
			, height(0)
		{
		}
	};

public:

	explicit ImageScreenerApp(
		HINSTANCE hInst)
		: hInst_(hInst)
		, hWnd_(nullptr)

		, display_(nullptr)

		, openBtn_(nullptr)
		, saveDirBtn_(nullptr)
		, prevBtn_(nullptr)
		, pauseBtn_(nullptr)
		, nextBtn_(nullptr)
		, saveBtn_(nullptr)

		, speedLabel_(nullptr)
		, speedSlider_(nullptr)

		, infoLabel_(nullptr)
		, savedLabel_(nullptr)
		, progressLabel_(nullptr)
		, saveDirLabel_(nullptr)
		, pixelInfoLabel_(nullptr)

		, recursiveCheck_(nullptr)
		, autoSkipCheck_(nullptr)

		, pixelTooltip_(nullptr)

		, font_(nullptr)
		, boldFont_(nullptr)

		, scanPending_(false)
		, decodeThreadExit_(false)
		, scanThreadExit_(false)
		, pixelThreadExit_(false)

		, oldDisplayProc_(nullptr)

		, lastPixelX_(-1)
		, lastPixelY_(-1)
		, lastPixelUpdateTick_(0)

		, pixelPending_(false)
		, pixelPendingX_(0)
		, pixelPendingY_(0)
		, pixelPendingRequestId_(0)
	{
		currentDecodePending_ = false;
	}

	~ImageScreenerApp()
	{
		shuttingDown_ = true;

		StopTimer();

		if (hWnd_)
		{
			KillTimer(
				hWnd_,
				TIMER_RESIZE);
		}

		StopWorkers();

		{
			std::lock_guard<std::mutex> lock(
				bitmapMutex_);

			DeleteBitmapData(
				currentBitmap_);

			DeleteBitmapData(
				preloadNextBitmap_);
		}

		if (font_)
		{
			DeleteObject(font_);
			font_ = nullptr;
		}

		if (boldFont_)
		{
			DeleteObject(boldFont_);
			boldFont_ = nullptr;
		}
	}

	bool Create()
	{
		WNDCLASSEXW wc;

		ZeroMemory(
			&wc,
			sizeof(wc));

		wc.cbSize =
			sizeof(wc);

		wc.hInstance =
			hInst_;

		wc.lpfnWndProc =
			&ImageScreenerApp::WndProc;

		wc.lpszClassName =
			L"ImageScreenerNativeWindow";

		wc.hCursor =
			LoadCursor(
				nullptr,
				IDC_ARROW);

		wc.hbrBackground =
			reinterpret_cast<HBRUSH>(
				COLOR_WINDOW + 1);

		wc.style =
			CS_HREDRAW |
			CS_VREDRAW;

		if (!RegisterClassExW(&wc))
		{
			if (
				GetLastError() !=
				ERROR_CLASS_ALREADY_EXISTS)
			{
				return false;
			}
		}

		hWnd_ =
			CreateWindowExW(
				0,
				wc.lpszClassName,
				L"AI视觉图片筛查工具",
				WS_OVERLAPPEDWINDOW |
				WS_CLIPCHILDREN,
				CW_USEDEFAULT,
				CW_USEDEFAULT,
				1200,
				900,
				nullptr,
				nullptr,
				hInst_,
				this);

		if (!hWnd_)
			return false;

		BuildUi();

		if (!StartWorkers())
		{
			MessageBoxW(
				hWnd_,
				L"后台工作线程创建失败。",
				L"错误",
				MB_ICONERROR);

			DestroyWindow(
				hWnd_);

			hWnd_ = nullptr;

			return false;
		}

		ShowWindow(
			hWnd_,
			SW_SHOW);

		UpdateWindow(
			hWnd_);

		ResizeUi(
			1200,
			900);

		UpdateUi();

		return true;
	}

	HWND GetHwnd() const
	{
		return hWnd_;
	}

private:

	// ========================================================
	// Window Proc
	// ========================================================

	static LRESULT CALLBACK WndProc(
		HWND hwnd,
		UINT msg,
		WPARAM wp,
		LPARAM lp)
	{
		ImageScreenerApp* app =
			reinterpret_cast<ImageScreenerApp*>(
				GetWindowLongPtrW(
					hwnd,
					GWLP_USERDATA));

		if (msg == WM_NCCREATE)
		{
			CREATESTRUCTW* cs =
				reinterpret_cast<CREATESTRUCTW*>(
					lp);

			app =
				static_cast<ImageScreenerApp*>(
					cs->lpCreateParams);

			SetWindowLongPtrW(
				hwnd,
				GWLP_USERDATA,
				reinterpret_cast<LONG_PTR>(
					app));

			app->hWnd_ =
				hwnd;
		}

		if (app)
		{
			return app->HandleMessage(
				msg,
				wp,
				lp);
		}

		return DefWindowProcW(
			hwnd,
			msg,
			wp,
			lp);
	}

	// ========================================================
	// Display subclass
	// ========================================================

	static LRESULT CALLBACK DisplayProc(
		HWND hwnd,
		UINT msg,
		WPARAM wp,
		LPARAM lp)
	{
		HWND parent =
			GetParent(hwnd);

		ImageScreenerApp* app =
			reinterpret_cast<ImageScreenerApp*>(
				parent
				? GetWindowLongPtrW(
					parent,
					GWLP_USERDATA)
				: 0);

		if (
			app &&
			msg == WM_MOUSEMOVE)
		{
			app->OnDisplayMouseMove(
				GET_X_LPARAM(lp),
				GET_Y_LPARAM(lp));
		}

		if (
			app &&
			app->oldDisplayProc_)
		{
			return CallWindowProcW(
				app->oldDisplayProc_,
				hwnd,
				msg,
				wp,
				lp);
		}

		return DefWindowProcW(
			hwnd,
			msg,
			wp,
			lp);
	}

	// ========================================================
	// 消息
	// ========================================================

	LRESULT HandleMessage(
		UINT msg,
		WPARAM wp,
		LPARAM lp)
	{
		switch (msg)
		{
		case WM_COMMAND:

			switch (LOWORD(wp))
			{
			case ID_BTN_OPEN:
				SelectFolder();
				return 0;

			case ID_BTN_SAVE_DIR:
				SelectSaveDir();
				return 0;

			case ID_BTN_PREV:
				PrevImage();
				return 0;

			case ID_BTN_PAUSE:
				TogglePause();
				return 0;

			case ID_BTN_NEXT:
				NextImage();
				return 0;

			case ID_BTN_SAVE:
				SaveImage();
				return 0;

			case ID_SPEED_UP:
				ChangeSpeed(+1);
				return 0;

			case ID_SPEED_DOWN:
				ChangeSpeed(-1);
				return 0;

			case ID_CHECK_RECURSIVE:
			{
				const bool checked =
					SendMessageW(
						recursiveCheck_,
						BM_GETCHECK,
						0,
						0) == BST_CHECKED;

				recursiveScan_ =
					checked;

				return 0;
			}

			case ID_CHECK_AUTOSKIP:
			{
				const bool checked =
					SendMessageW(
						autoSkipCheck_,
						BM_GETCHECK,
						0,
						0) == BST_CHECKED;

				autoSkipDecodeFailure_ =
					checked;

				return 0;
			}
			}

			break;

		case WM_HSCROLL:

			if (
				reinterpret_cast<HWND>(lp) ==
				speedSlider_)
			{
				speedTenths_ =
					static_cast<int>(
						SendMessageW(
							speedSlider_,
							TBM_GETPOS,
							0,
							0));

				if (!paused_)
					StartTimer();

				UpdateUi();
			}

			return 0;

		case WM_TIMER:

			if (wp == TIMER_PLAY)
			{
				if (
					!currentDecodePending_.load())
				{
					NextImage();
				}

				return 0;
			}

			if (wp == TIMER_RESIZE)
			{
				KillTimer(
					hWnd_,
					TIMER_RESIZE);

				if (FileCount() > 0)
				{
					QueueCurrentDecode(
						false);
				}

				return 0;
			}

			break;

		case WM_SIZE:

			ResizeUi(
				LOWORD(lp),
				HIWORD(lp));

			if (FileCount() > 0)
			{
				SetTimer(
					hWnd_,
					TIMER_RESIZE,
					200,
					nullptr);
			}

			return 0;

		case WM_DRAWITEM:
		{
			DRAWITEMSTRUCT* dis =
				reinterpret_cast<DRAWITEMSTRUCT*>(
					lp);

			if (
				dis &&
				dis->hwndItem == display_)
			{
				PaintDisplay(
					dis);

				return TRUE;
			}

			break;
		}

		case WM_NOTIFY:
		{
			NMHDR* hdr =
				reinterpret_cast<NMHDR*>(
					lp);

			if (
				hdr &&
				pixelTooltip_ &&
				hdr->hwndFrom == pixelTooltip_ &&
				hdr->code == TTN_GETDISPINFOW)
			{
				NMTTDISPINFOW* info =
					reinterpret_cast<NMTTDISPINFOW*>(
						lp);

				if (info)
				{
					info->lpszText =
						const_cast<LPWSTR>(
							fullPixelText_.c_str());

					return 0;
				}
			}

			break;
		}

		case WM_APP_SCAN_DONE:

			OnScanDone(
				static_cast<unsigned long>(
					wp));

			return 0;

		case WM_APP_DECODE_DONE:

			OnDecodeDone(
				static_cast<unsigned long>(
					wp));

			return 0;

		case WM_APP_PIXEL_DONE:

			OnPixelDone(
				static_cast<unsigned long>(
					wp));

			return 0;

		case WM_DESTROY:

			StopTimer();

			KillTimer(
				hWnd_,
				TIMER_RESIZE);

			shuttingDown_ = true;

			SignalWorkersToStop();

			PostQuitMessage(
				0);

			return 0;
		}

		return DefWindowProcW(
			hWnd_,
			msg,
			wp,
			lp);
	}

	// ========================================================
	// 绘制
	// ========================================================

	void PaintDisplay(
		DRAWITEMSTRUCT* dis)
	{
		if (!dis)
			return;

		HDC hdc =
			dis->hDC;

		RECT rc =
			dis->rcItem;

		HBRUSH bg =
			CreateSolidBrush(
				RGB(80, 80, 80));

		if (bg)
		{
			FillRect(
				hdc,
				&rc,
				bg);

			DeleteObject(
				bg);
		}

		std::lock_guard<std::mutex> lock(
			bitmapMutex_);

		if (currentBitmap_.hBitmap)
		{
			HDC mem =
				CreateCompatibleDC(
					hdc);

			if (!mem)
				return;

			HGDIOBJ old =
				SelectObject(
					mem,
					currentBitmap_.hBitmap);

			const int x =
				(rc.right -
					currentBitmap_.width) / 2;

			const int y =
				(rc.bottom -
					currentBitmap_.height) / 2;

			BitBlt(
				hdc,
				x,
				y,
				currentBitmap_.width,
				currentBitmap_.height,
				mem,
				0,
				0,
				SRCCOPY);

			SelectObject(
				mem,
				old);

			DeleteDC(
				mem);

			return;
		}

		bool empty = false;

		{
			std::lock_guard<std::mutex> fileLock(
				filesMutex_);

			empty =
				files_.empty();
		}

		SetBkMode(
			hdc,
			TRANSPARENT);

		SetTextColor(
			hdc,
			RGB(230, 230, 230));

		const wchar_t* text =
			nullptr;

		if (empty)
		{
			text =
				L"请点击打开文件夹加载图片";
		}
		else if (decodeFailed_)
		{
			text =
				L"图片加载失败";
		}
		else
		{
			text =
				L"正在加载图片...";
		}

		DrawTextW(
			hdc,
			text,
			-1,
			&rc,
			DT_CENTER |
			DT_VCENTER |
			DT_SINGLELINE);
	}

	// ========================================================
	// 鼠标像素
	// ========================================================

	void OnDisplayMouseMove(
		int mx,
		int my)
	{
		if (
			!display_ ||
			!pixelInfoLabel_)
		{
			return;
		}

		RECT rc;

		if (
			!GetClientRect(
				display_,
				&rc))
		{
			return;
		}

		int drawW = 0;
		int drawH = 0;
		int sourceW = 0;
		int sourceH = 0;

		int imageX = 0;
		int imageY = 0;

		int sourceX = 0;
		int sourceY = 0;

		// ----------------------------------------------------
		// 先读取当前显示 Bitmap
		// ----------------------------------------------------

		{
			std::lock_guard<std::mutex> lock(
				bitmapMutex_);

			if (
				!currentBitmap_.hBitmap ||
				!currentBitmap_.bits ||
				currentBitmap_.width <= 0 ||
				currentBitmap_.height <= 0)
			{
				SetWindowTextW(
					pixelInfoLabel_,
					L"像素: 无");

				return;
			}

			drawW =
				currentBitmap_.width;

			drawH =
				currentBitmap_.height;

			sourceW =
				currentBitmap_.sourceWidth;

			sourceH =
				currentBitmap_.sourceHeight;

			const int offsetX =
				(rc.right - drawW) / 2;

			const int offsetY =
				(rc.bottom - drawH) / 2;

			imageX =
				mx - offsetX;

			imageY =
				my - offsetY;

			if (
				imageX < 0 ||
				imageX >= drawW ||
				imageY < 0 ||
				imageY >= drawH)
			{
				return;
			}

			// -----------------------------------------------
			// 显示 Bitmap 的像素
			// BGRA
			// -----------------------------------------------

			unsigned char* bits =
				static_cast<unsigned char*>(
					currentBitmap_.bits);

			const int stride =
				drawW * 4;

			unsigned char* pixel =
				bits +
				imageY * stride +
				imageX * 4;

			const int b =
				static_cast<int>(
					pixel[0]);

			const int g =
				static_cast<int>(
					pixel[1]);

			const int r =
				static_cast<int>(
					pixel[2]);

			const int a =
				static_cast<int>(
					pixel[3]);

			// -----------------------------------------------
			// 原图坐标
			// -----------------------------------------------

			sourceX =
				imageX;

			sourceY =
				imageY;

			if (
				sourceW > 0 &&
				sourceH > 0)
			{
				sourceX =
					static_cast<int>(
					(
						static_cast<double>(
							imageX) *
						sourceW
						) /
						drawW);

				sourceY =
					static_cast<int>(
					(
						static_cast<double>(
							imageY) *
						sourceH
						) /
						drawH);

				sourceX =
					ClampValue(
						sourceX,
						0,
						sourceW - 1);

				sourceY =
					ClampValue(
						sourceY,
						0,
						sourceH - 1);
			}

			// -----------------------------------------------
			// 立即显示 Gray
			// -----------------------------------------------

			const int gray =
				static_cast<int>(
					0.299 * r +
					0.587 * g +
					0.114 * b +
					0.5);

			wchar_t buf[512];

			swprintf_s(
				buf,
				_countof(buf),
				L"显示坐标:(%d,%d) | 原图坐标:(%d,%d) | "
				L"RGBA:(%d,%d,%d,%d) | Gray:%d | HEX:#%02X%02X%02X",
				imageX,
				imageY,
				sourceX,
				sourceY,
				r,
				g,
				b,
				a,
				gray,
				r,
				g,
				b);

			fullPixelText_ =
				buf;
		}

		// ----------------------------------------------------
		// 立即更新 UI
		// ----------------------------------------------------

		const std::wstring displayText =
			BuildEllipsizedPixelText(
				fullPixelText_);

		SetWindowTextW(
			pixelInfoLabel_,
			displayText.c_str());

		UpdatePixelTooltip();

		// ----------------------------------------------------
		// 原图 WIC 真实像素后台查询
		// ----------------------------------------------------

		if (
			sourceW <= 0 ||
			sourceH <= 0)
		{
			return;
		}

		const ULONGLONG now =
			GetTickCount64();

		if (
			imageX == lastPixelX_ &&
			imageY == lastPixelY_)
		{
			return;
		}

		if (
			now -
			lastPixelUpdateTick_ < 30)
		{
			return;
		}

		lastPixelX_ =
			imageX;

		lastPixelY_ =
			imageY;

		lastPixelUpdateTick_ =
			now;

		std::wstring path =
			CurrentPath();

		if (path.empty())
			return;

		QueuePixelQuery(
			path,
			sourceX,
			sourceY);
	}

	// ========================================================
	// Pixel query
	// ========================================================

	void QueuePixelQuery(
		const std::wstring& path,
		int x,
		int y)
	{
		if (shuttingDown_)
			return;

		const unsigned long request =
			++pixelRequestId_;

		{
			std::lock_guard<std::mutex> lock(
				pixelMutex_);

			pixelPendingPath_ =
				path;

			pixelPendingX_ =
				x;

			pixelPendingY_ =
				y;

			pixelPendingRequestId_ =
				request;

			pixelPending_ =
				true;
		}

		pixelCv_.notify_one();
	}

	// ========================================================
	// Pixel done
	// ========================================================

	void OnPixelDone(
		unsigned long request)
	{
		if (shuttingDown_)
			return;

		if (
			request !=
			pixelRequestId_.load())
		{
			return;
		}

		PixelResult result;

		{
			std::lock_guard<std::mutex> lock(
				pixelResultMutex_);

			if (
				!pixelResult_.valid ||
				pixelResult_.requestId !=
				request)
			{
				return;
			}

			result =
				pixelResult_;
		}

		if (!result.valid)
			return;

		int displayX = 0;
		int displayY = 0;

		{
			std::lock_guard<std::mutex> lock(
				bitmapMutex_);

			if (
				currentBitmap_.width <= 0 ||
				currentBitmap_.height <= 0 ||
				currentBitmap_.sourceWidth <= 0 ||
				currentBitmap_.sourceHeight <= 0)
			{
				return;
			}

			displayX =
				static_cast<int>(
				(
					static_cast<double>(
						result.sourceX) *
					currentBitmap_.width
					) /
					currentBitmap_.sourceWidth);

			displayY =
				static_cast<int>(
				(
					static_cast<double>(
						result.sourceY) *
					currentBitmap_.height
					) /
					currentBitmap_.sourceHeight);

			displayX =
				ClampValue(
					displayX,
					0,
					currentBitmap_.width - 1);

			displayY =
				ClampValue(
					displayY,
					0,
					currentBitmap_.height - 1);
		}

		wchar_t buf[512];

		swprintf_s(
			buf,
			_countof(buf),
			L"显示坐标:(%d,%d) | 原图坐标:(%d,%d) | "
			L"RGBA:(%d,%d,%d,%d) | Gray:%d | HEX:#%02X%02X%02X",
			displayX,
			displayY,
			result.sourceX,
			result.sourceY,
			result.r,
			result.g,
			result.b,
			result.a,
			result.gray,
			result.r,
			result.g,
			result.b);

		fullPixelText_ =
			buf;

		const std::wstring displayText =
			BuildEllipsizedPixelText(
				fullPixelText_);

		SetWindowTextW(
			pixelInfoLabel_,
			displayText.c_str());

		UpdatePixelTooltip();
	}

	// ========================================================
	// 像素文字自动省略
	// ========================================================

	std::wstring BuildEllipsizedPixelText(
		const std::wstring& text)
	{
		if (
			text.empty() ||
			!pixelInfoLabel_)
		{
			return text;
		}

		RECT rc;

		if (
			!GetClientRect(
				pixelInfoLabel_,
				&rc))
		{
			return text;
		}

		const int availableWidth =
			rc.right - rc.left;

		if (availableWidth <= 20)
			return text;

		HDC hdc =
			GetDC(
				pixelInfoLabel_);

		if (!hdc)
			return text;

		HFONT oldFont =
			nullptr;

		if (boldFont_)
		{
			oldFont =
				reinterpret_cast<HFONT>(
					SelectObject(
						hdc,
						boldFont_));
		}

		SIZE size;

		if (
			GetTextExtentPoint32W(
				hdc,
				text.c_str(),
				static_cast<int>(
					text.size()),
				&size) &&
			size.cx <= availableWidth)
		{
			if (oldFont)
			{
				SelectObject(
					hdc,
					oldFont);
			}

			ReleaseDC(
				pixelInfoLabel_,
				hdc);

			return text;
		}

		const std::wstring ellipsis =
			L"...";

		int low = 0;
		int high =
			static_cast<int>(
				text.size());

		int best = 0;

		while (low <= high)
		{
			const int mid =
				low +
				(high - low) / 2;

			std::wstring candidate =
				text.substr(
					0,
					static_cast<size_t>(
						mid)) +
				ellipsis;

			SIZE candidateSize;

			if (
				!GetTextExtentPoint32W(
					hdc,
					candidate.c_str(),
					static_cast<int>(
						candidate.size()),
					&candidateSize))
			{
				break;
			}

			if (
				candidateSize.cx <=
				availableWidth)
			{
				best =
					mid;

				low =
					mid + 1;
			}
			else
			{
				high =
					mid - 1;
			}
		}

		if (oldFont)
		{
			SelectObject(
				hdc,
				oldFont);
		}

		ReleaseDC(
			pixelInfoLabel_,
			hdc);

		if (best <= 0)
			return ellipsis;

		return
			text.substr(
				0,
				static_cast<size_t>(
					best)) +
			ellipsis;
	}

	// ========================================================
	// ToolTip
	// ========================================================

	void CreatePixelTooltip()
	{
		if (
			pixelTooltip_ ||
			!hWnd_ ||
			!display_)
		{
			return;
		}

		pixelTooltip_ =
			CreateWindowExW(
				WS_EX_TOPMOST,
				TOOLTIPS_CLASSW,
				nullptr,
				WS_POPUP |
				TTS_ALWAYSTIP |
				TTS_NOPREFIX,
				CW_USEDEFAULT,
				CW_USEDEFAULT,
				CW_USEDEFAULT,
				CW_USEDEFAULT,
				hWnd_,
				nullptr,
				hInst_,
				nullptr);

		if (!pixelTooltip_)
			return;

		TOOLINFOW ti;

		ZeroMemory(
			&ti,
			sizeof(ti));

		ti.cbSize =
			sizeof(ti);

		ti.uFlags =
			TTF_IDISHWND |
			TTF_SUBCLASS;

		ti.hwnd =
			hWnd_;

		ti.uId =
			reinterpret_cast<UINT_PTR>(
				display_);

		ti.lpszText =
			LPSTR_TEXTCALLBACKW;

		SendMessageW(
			pixelTooltip_,
			TTM_ADDTOOLW,
			0,
			reinterpret_cast<LPARAM>(
				&ti));

		SendMessageW(
			pixelTooltip_,
			TTM_SETMAXTIPWIDTH,
			0,
			900);

		SendMessageW(
			pixelTooltip_,
			TTM_SETDELAYTIME,
			TTDT_INITIAL,
			350);

		SendMessageW(
			pixelTooltip_,
			TTM_SETDELAYTIME,
			TTDT_RESHOW,
			100);
	}

	void UpdatePixelTooltip()
	{
		if (!pixelTooltip_)
			return;

		SendMessageW(
			pixelTooltip_,
			TTM_UPDATE,
			0,
			0);
	}

	// ========================================================
	// UI Build
	// ========================================================

	void BuildUi()
	{
		font_ =
			CreateFontW(
				-16,
				0,
				0,
				0,
				FW_NORMAL,
				FALSE,
				FALSE,
				FALSE,
				DEFAULT_CHARSET,
				OUT_DEFAULT_PRECIS,
				CLIP_DEFAULT_PRECIS,
				CLEARTYPE_QUALITY,
				DEFAULT_PITCH |
				FF_DONTCARE,
				L"Microsoft YaHei UI");

		boldFont_ =
			CreateFontW(
				-16,
				0,
				0,
				0,
				FW_SEMIBOLD,
				FALSE,
				FALSE,
				FALSE,
				DEFAULT_CHARSET,
				OUT_DEFAULT_PRECIS,
				CLIP_DEFAULT_PRECIS,
				CLEARTYPE_QUALITY,
				DEFAULT_PITCH |
				FF_DONTCARE,
				L"Microsoft YaHei UI");

		// ====================================================
		// Display
		//
		// SS_NOTIFY 用于增强鼠标交互。
		// ====================================================

		display_ =
			CreateWindowExW(
				WS_EX_CLIENTEDGE,
				L"STATIC",
				nullptr,
				WS_CHILD |
				WS_VISIBLE |
				SS_OWNERDRAW |
				SS_NOTIFY,
				5,
				5,
				1180,
				650,
				hWnd_,
				nullptr,
				hInst_,
				nullptr);

		if (display_)
		{
			oldDisplayProc_ =
				reinterpret_cast<WNDPROC>(
					SetWindowLongPtrW(
						display_,
						GWLP_WNDPROC,
						reinterpret_cast<LONG_PTR>(
							&ImageScreenerApp::DisplayProc)));
		}

		// ====================================================
		// 打开目录
		// ====================================================

		openBtn_ =
			CreateWindowW(
				L"BUTTON",
				L"打开文件夹",
				WS_CHILD |
				WS_VISIBLE |
				BS_PUSHBUTTON,
				0,
				0,
				110,
				32,
				hWnd_,
				reinterpret_cast<HMENU>(
					ID_BTN_OPEN),
				hInst_,
				nullptr);

		// ====================================================
		// 保存目录
		// ====================================================

		saveDirBtn_ =
			CreateWindowW(
				L"BUTTON",
				L"选择保存目录",
				WS_CHILD |
				WS_VISIBLE |
				BS_PUSHBUTTON,
				0,
				0,
				120,
				32,
				hWnd_,
				reinterpret_cast<HMENU>(
					ID_BTN_SAVE_DIR),
				hInst_,
				nullptr);

		// ====================================================
		// Prev
		// ====================================================

		prevBtn_ =
			CreateWindowW(
				L"BUTTON",
				L"上一张",
				WS_CHILD |
				WS_VISIBLE |
				BS_PUSHBUTTON,
				0,
				0,
				100,
				34,
				hWnd_,
				reinterpret_cast<HMENU>(
					ID_BTN_PREV),
				hInst_,
				nullptr);

		// ====================================================
		// Pause
		// ====================================================

		pauseBtn_ =
			CreateWindowW(
				L"BUTTON",
				L"暂停/继续",
				WS_CHILD |
				WS_VISIBLE |
				BS_PUSHBUTTON,
				0,
				0,
				120,
				34,
				hWnd_,
				reinterpret_cast<HMENU>(
					ID_BTN_PAUSE),
				hInst_,
				nullptr);

		// ====================================================
		// Next
		// ====================================================

		nextBtn_ =
			CreateWindowW(
				L"BUTTON",
				L"下一张",
				WS_CHILD |
				WS_VISIBLE |
				BS_PUSHBUTTON,
				0,
				0,
				100,
				34,
				hWnd_,
				reinterpret_cast<HMENU>(
					ID_BTN_NEXT),
				hInst_,
				nullptr);

		// ====================================================
		// Save
		// ====================================================

		saveBtn_ =
			CreateWindowW(
				L"BUTTON",
				L"保存",
				WS_CHILD |
				WS_VISIBLE |
				BS_PUSHBUTTON,
				0,
				0,
				100,
				34,
				hWnd_,
				reinterpret_cast<HMENU>(
					ID_BTN_SAVE),
				hInst_,
				nullptr);

		// ====================================================
		// Speed label
		// ====================================================

		speedLabel_ =
			CreateWindowW(
				L"STATIC",
				L"间隔: 1.0 秒",
				WS_CHILD |
				WS_VISIBLE,
				0,
				0,
				100,
				26,
				hWnd_,
				nullptr,
				hInst_,
				nullptr);

		// ====================================================
		// Speed slider
		// ====================================================

		speedSlider_ =
			CreateWindowExW(
				0,
				TRACKBAR_CLASSW,
				nullptr,
				WS_CHILD |
				WS_VISIBLE |
				TBS_AUTOTICKS,
				0,
				0,
				180,
				35,
				hWnd_,
				reinterpret_cast<HMENU>(
					ID_SPEED),
				hInst_,
				nullptr);

		SendMessageW(
			speedSlider_,
			TBM_SETRANGE,
			TRUE,
			MAKELONG(1, 50));

		SendMessageW(
			speedSlider_,
			TBM_SETPOS,
			TRUE,
			speedTenths_);

		SendMessageW(
			speedSlider_,
			TBM_SETTICFREQ,
			5,
			0);

		// ====================================================
		// Recursive
		// ====================================================

		recursiveCheck_ =
			CreateWindowW(
				L"BUTTON",
				L"递归子文件夹",
				WS_CHILD |
				WS_VISIBLE |
				BS_AUTOCHECKBOX,
				0,
				0,
				130,
				28,
				hWnd_,
				reinterpret_cast<HMENU>(
					ID_CHECK_RECURSIVE),
				hInst_,
				nullptr);

		SendMessageW(
			recursiveCheck_,
			BM_SETCHECK,
			BST_CHECKED,
			0);

		// ====================================================
		// Auto skip
		// ====================================================

		autoSkipCheck_ =
			CreateWindowW(
				L"BUTTON",
				L"失败自动跳过",
				WS_CHILD |
				WS_VISIBLE |
				BS_AUTOCHECKBOX,
				0,
				0,
				130,
				28,
				hWnd_,
				reinterpret_cast<HMENU>(
					ID_CHECK_AUTOSKIP),
				hInst_,
				nullptr);

		SendMessageW(
			autoSkipCheck_,
			BM_SETCHECK,
			BST_CHECKED,
			0);

		// ====================================================
		// Info
		// ====================================================

		infoLabel_ =
			CreateWindowW(
				L"STATIC",
				L"请点击打开文件夹加载图片",
				WS_CHILD |
				WS_VISIBLE |
				SS_LEFT |
				SS_PATHELLIPSIS,
				0,
				0,
				300,
				26,
				hWnd_,
				reinterpret_cast<HMENU>(
					ID_LABEL_INFO),
				hInst_,
				nullptr);

		// ====================================================
		// Saved
		// ====================================================

		savedLabel_ =
			CreateWindowW(
				L"STATIC",
				L"已保存: 0",
				WS_CHILD |
				WS_VISIBLE,
				0,
				0,
				120,
				26,
				hWnd_,
				reinterpret_cast<HMENU>(
					ID_LABEL_SAVED),
				hInst_,
				nullptr);

		// ====================================================
		// Save dir
		// ====================================================

		saveDirLabel_ =
			CreateWindowW(
				L"STATIC",
				L"保存目录: 未设置",
				WS_CHILD |
				WS_VISIBLE |
				SS_LEFT |
				SS_PATHELLIPSIS,
				0,
				0,
				300,
				26,
				hWnd_,
				reinterpret_cast<HMENU>(
					ID_LABEL_SAVE_DIR),
				hInst_,
				nullptr);

		// ====================================================
		// Progress
		// ====================================================

		progressLabel_ =
			CreateWindowW(
				L"STATIC",
				L"0 / 0",
				WS_CHILD |
				WS_VISIBLE,
				0,
				0,
				160,
				32,
				hWnd_,
				reinterpret_cast<HMENU>(
					ID_LABEL_PROGRESS),
				hInst_,
				nullptr);

		// ====================================================
		// Pixel
		// ====================================================

		pixelInfoLabel_ =
			CreateWindowW(
				L"STATIC",
				L"像素: 无",
				WS_CHILD |
				WS_VISIBLE |
				SS_LEFT |
				SS_PATHELLIPSIS,
				0,
				0,
				700,
				30,
				hWnd_,
				reinterpret_cast<HMENU>(
					ID_LABEL_PIXEL),
				hInst_,
				nullptr);

		fullPixelText_ =
			L"像素: 无";

		CreatePixelTooltip();

		// ====================================================
		// Fonts
		// ====================================================

		SetControlFont(openBtn_);
		SetControlFont(saveDirBtn_);

		SetControlFont(prevBtn_);
		SetControlFont(pauseBtn_);
		SetControlFont(nextBtn_);
		SetControlFont(saveBtn_);

		SetControlFont(speedLabel_);
		SetControlFont(speedSlider_);

		SetControlFont(recursiveCheck_);
		SetControlFont(autoSkipCheck_);

		SetControlFont(infoLabel_);
		SetControlFont(savedLabel_);
		SetControlFont(saveDirLabel_);

		SetControlFont(
			progressLabel_,
			true);

		SetControlFont(
			pixelInfoLabel_,
			true);

		EnableNavigation(
			false);
	}

	void SetControlFont(
		HWND ctrl,
		bool bold = false)
	{
		if (!ctrl)
			return;

		SendMessageW(
			ctrl,
			WM_SETFONT,
			reinterpret_cast<WPARAM>(
				bold
				? boldFont_
				: font_),
			TRUE);
	}

	// ========================================================
	// Resize
	// ========================================================

	void ResizeUi(
		int w,
		int h)
	{
		if (!display_)
			return;

		w =
			(std::max)(320, w);

		h =
			(std::max)(320, h);

		const int bottomAreaH =
			150;

		int displayH =
			h - bottomAreaH;

		if (displayH < 180)
			displayH = 180;

		MoveWindow(
			display_,
			5,
			5,
			(std::max)(
				100,
				w - 10),
			displayH,
			TRUE);

		const int row1Y =
			displayH + 12;

		const int row2Y =
			row1Y + 38;

		const int row3Y =
			row2Y + 40;

		// ====================================================
		// Row 1
		// ====================================================

		MoveWindow(
			speedLabel_,
			10,
			row1Y + 4,
			72,
			24,
			TRUE);

		MoveWindow(
			speedSlider_,
			82,
			row1Y,
			160,
			32,
			TRUE);

		MoveWindow(
			recursiveCheck_,
			245,
			row1Y + 2,
			130,
			28,
			TRUE);

		MoveWindow(
			autoSkipCheck_,
			375,
			row1Y + 2,
			125,
			28,
			TRUE);

		MoveWindow(
			openBtn_,
			505,
			row1Y,
			105,
			32,
			TRUE);

		MoveWindow(
			saveDirBtn_,
			615,
			row1Y,
			115,
			32,
			TRUE);

		MoveWindow(
			savedLabel_,
			740,
			row1Y + 4,
			105,
			25,
			TRUE);

		const int savePathX =
			850;

		const int savePathW =
			(std::max)(
				120,
				w -
				savePathX -
				10);

		MoveWindow(
			saveDirLabel_,
			savePathX,
			row1Y + 4,
			savePathW,
			25,
			TRUE);

		// ====================================================
		// Row 2
		// ====================================================

		MoveWindow(
			progressLabel_,
			10,
			row2Y + 3,
			150,
			30,
			TRUE);

		const int buttonW1 =
			95;

		const int pauseW =
			110;

		const int gap =
			10;

		const int totalW =
			buttonW1 +
			gap +
			pauseW +
			gap +
			buttonW1 +
			gap +
			buttonW1;

		int left =
			(w - totalW) / 2;

		if (left < 170)
			left = 170;

		MoveWindow(
			prevBtn_,
			left,
			row2Y,
			buttonW1,
			34,
			TRUE);

		MoveWindow(
			pauseBtn_,
			left +
			buttonW1 +
			gap,
			row2Y,
			pauseW,
			34,
			TRUE);

		MoveWindow(
			nextBtn_,
			left +
			buttonW1 +
			gap +
			pauseW +
			gap,
			row2Y,
			buttonW1,
			34,
			TRUE);

		MoveWindow(
			saveBtn_,
			left +
			2 *
			(buttonW1 + gap) +
			pauseW +
			gap,
			row2Y,
			buttonW1,
			34,
			TRUE);

		// ====================================================
		// Row 3 Pixel
		// ====================================================

		MoveWindow(
			pixelInfoLabel_,
			10,
			row3Y + 2,
			(std::max)(
				300,
				w - 20),
			30,
			TRUE);

		if (!fullPixelText_.empty())
		{
			const std::wstring text =
				BuildEllipsizedPixelText(
					fullPixelText_);

			SetWindowTextW(
				pixelInfoLabel_,
				text.c_str());
		}

		UpdatePixelTooltip();
	}

	// ========================================================
	// Select folder
	// ========================================================

	void SelectFolder()
	{
		const std::wstring folder =
			PickFolder(
				L"选择包含图片的文件夹");

		if (folder.empty())
			return;

		StopTimer();

		++scanRequestId_;
		++decodeRequestId_;
		++preloadRequestId_;
		++pixelRequestId_;

		currentDecodePending_ =
			false;

		ClearPixelRequest();
		ClearDecodeQueue();
		ClearBitmaps();

		{
			std::lock_guard<std::mutex> lock(
				filesMutex_);

			files_.clear();
		}

		currentIndex_ = 0;

		savedCount_ = 0;
		savedFiles_.clear();

		paused_ = false;
		decodeFailed_ = false;
		consecutiveDecodeFailures_ = 0;

		ResetPixelState();

		{
			std::lock_guard<std::mutex> lock(
				saveDirMutex_);

			currentFolder_ =
				folder;

			saveDir_ =
				folder +
				L"\\saved";
		}

		std::wstring defaultSaveDir;

		{
			std::lock_guard<std::mutex> lock(
				saveDirMutex_);

			defaultSaveDir =
				saveDir_;
		}

		CreateDirectoryRecursive(
			defaultSaveDir);

		LoadSavedState(
			defaultSaveDir);

		UpdateSaveDirLabel();

		SetWindowTextW(
			infoLabel_,
			L"正在扫描图片...");

		SetWindowTextW(
			progressLabel_,
			L"0 / 0");

		UpdateSavedLabel();

		EnableNavigation(
			false);

		InvalidateRect(
			display_,
			nullptr,
			FALSE);

		const unsigned long request =
			scanRequestId_.load();

		{
			std::lock_guard<std::mutex> lock(
				scanMutex_);

			scanFolder_ =
				folder;

			scanPending_ =
				true;

			scanPendingRequestId_ =
				request;
		}

		scanCv_.notify_one();
	}

	// ========================================================
	// Scan done
	// ========================================================

	void OnScanDone(
		unsigned long request)
	{
		if (shuttingDown_)
			return;

		if (
			request !=
			scanRequestId_.load())
		{
			return;
		}

		const size_t count =
			FileCount();

		if (count == 0)
		{
			MessageBoxW(
				hWnd_,
				L"所选文件夹中没有支持的图片文件！",
				L"错误",
				MB_ICONERROR);

			SetWindowTextW(
				infoLabel_,
				L"没有找到图片");

			EnableNavigation(
				false);

			return;
		}

		currentIndex_ = 0;

		paused_ = false;
		decodeFailed_ = false;
		consecutiveDecodeFailures_ = 0;

		EnableNavigation(
			true);

		UpdateUi();

		QueueCurrentDecode(
			true);

		StartTimer();
	}

	// ========================================================
	// Decode size
	// ========================================================

	void GetDecodeSize(
		UINT& w,
		UINT& h)
	{
		w = 900;
		h = 600;

		RECT rc;

		if (
			display_ &&
			GetClientRect(
				display_,
				&rc))
		{
			if (
				rc.right > 10 &&
				rc.bottom > 10)
			{
				w =
					static_cast<UINT>(
						rc.right - 10);

				h =
					static_cast<UINT>(
						rc.bottom - 10);
			}
		}
	}

	// ========================================================
	// Current decode
	// ========================================================

	void QueueCurrentDecode(
		bool clearCurrent)
	{
		const std::wstring path =
			CurrentPath();

		if (path.empty())
		{
			currentDecodePending_ =
				false;

			return;
		}

		const unsigned long request =
			++decodeRequestId_;

		++preloadRequestId_;

		++pixelRequestId_;

		ClearPixelRequest();
		ClearDecodeQueue();
		ClearPreloadBitmaps();

		if (clearCurrent)
		{
			std::lock_guard<std::mutex> lock(
				bitmapMutex_);

			DeleteBitmapData(
				currentBitmap_);

			decodeFailed_ =
				false;
		}

		UINT width = 0;
		UINT height = 0;

		GetDecodeSize(
			width,
			height);

		DecodeTask task;

		task.kind =
			DecodeTaskKind::Current;

		task.requestId =
			request;

		task.path =
			path;

		task.width =
			width;

		task.height =
			height;

		currentDecodePending_ =
			true;

		QueueDecodeTask(
			task);

		InvalidateRect(
			display_,
			nullptr,
			FALSE);
	}

	void QueueCurrentDecodeWithRequest(
		unsigned long request)
	{
		const std::wstring path =
			CurrentPath();

		if (path.empty())
		{
			currentDecodePending_ =
				false;

			return;
		}

		ClearDecodeQueue();
		ClearPreloadBitmaps();

		UINT width = 0;
		UINT height = 0;

		GetDecodeSize(
			width,
			height);

		DecodeTask task;

		task.kind =
			DecodeTaskKind::Current;

		task.requestId =
			request;

		task.path =
			path;

		task.width =
			width;

		task.height =
			height;

		currentDecodePending_ =
			true;

		QueueDecodeTask(
			task);
	}

	// ========================================================
	// Decode done
	// ========================================================

	void OnDecodeDone(
		unsigned long request)
	{
		if (shuttingDown_)
			return;

		if (
			request !=
			decodeRequestId_.load())
		{
			return;
		}

		currentDecodePending_ =
			false;

		if (decodeFailed_)
		{
			++consecutiveDecodeFailures_;

			const size_t count =
				FileCount();

			if (
				autoSkipDecodeFailure_.load() &&
				!paused_ &&
				count > 0)
			{
				if (
					consecutiveDecodeFailures_ <
					count)
				{
					NextImage();

					return;
				}

				paused_ =
					true;

				StopTimer();

				SetWindowTextW(
					infoLabel_,
					L"连续图片加载失败，已自动暂停");
			}

			QueuePreloadAroundCurrent();

			InvalidateRect(
				display_,
				nullptr,
				FALSE);

			UpdateUi();

			return;
		}

		consecutiveDecodeFailures_ =
			0;

		ResetPixelState();

		InvalidateRect(
			display_,
			nullptr,
			FALSE);

		const std::wstring path =
			CurrentPath();

		if (!path.empty())
		{
			const std::wstring title =
				L"AI视觉图片筛查工具 - C++原生版 - " +
				GetFileNameOnly(path);

			SetWindowTextW(
				hWnd_,
				title.c_str());
		}

		QueuePreloadAroundCurrent();

		UpdateUi();
	}

	// ========================================================
	// Preload next
	// ========================================================

	void QueuePreloadAroundCurrent()
	{
		const size_t count =
			FileCount();

		if (count < 2)
			return;

		std::wstring nextPath;

		{
			std::lock_guard<std::mutex> lock(
				filesMutex_);

			if (
				files_.empty() ||
				currentIndex_ >= files_.size())
			{
				return;
			}

			const size_t nextIndex =
				(currentIndex_ + 1) %
				count;

			nextPath =
				files_[nextIndex];
		}

		const unsigned long request =
			++preloadRequestId_;

		ClearDecodeQueue();

		{
			std::lock_guard<std::mutex> lock(
				bitmapMutex_);

			DeleteBitmapData(
				preloadNextBitmap_);

			preloadNextPath_.clear();
		}

		UINT width = 0;
		UINT height = 0;

		GetDecodeSize(
			width,
			height);

		DecodeTask task;

		task.kind =
			DecodeTaskKind::PreloadNext;

		task.requestId =
			request;

		task.path =
			nextPath;

		task.width =
			width;

		task.height =
			height;

		QueueDecodeTask(
			task);
	}

	// ========================================================
	// Navigation
	// ========================================================

	void NextImage()
	{
		const size_t count =
			FileCount();

		if (count == 0)
			return;

		currentIndex_ =
			(currentIndex_ + 1) %
			count;

		ShowIndexedImage(
			+1);
	}

	void PrevImage()
	{
		const size_t count =
			FileCount();

		if (count == 0)
			return;

		currentIndex_ =
			(currentIndex_ + count - 1) %
			count;

		ShowIndexedImage(
			-1);
	}

	void ShowIndexedImage(
		int direction)
	{
		const std::wstring path =
			CurrentPath();

		if (path.empty())
			return;

		++decodeRequestId_;
		++preloadRequestId_;
		++pixelRequestId_;

		ClearPixelRequest();
		ClearDecodeQueue();

		bool usedPreload =
			false;

		{
			std::lock_guard<std::mutex> lock(
				bitmapMutex_);

			if (
				direction > 0 &&
				preloadNextBitmap_.hBitmap &&
				preloadNextPath_ == path)
			{
				MoveBitmapData(
					currentBitmap_,
					preloadNextBitmap_);

				preloadNextPath_.clear();

				decodeFailed_ =
					false;

				currentDecodePending_ =
					false;

				usedPreload =
					true;
			}
			else
			{
				DeleteBitmapData(
					currentBitmap_);

				decodeFailed_ =
					false;

				currentDecodePending_ =
					false;
			}
		}

		ClearPreloadBitmaps();

		consecutiveDecodeFailures_ =
			0;

		ResetPixelState();

		InvalidateRect(
			display_,
			nullptr,
			FALSE);

		UpdateUi();

		if (usedPreload)
		{
			QueuePreloadAroundCurrent();
		}
		else
		{
			QueueCurrentDecodeWithRequest(
				decodeRequestId_.load());
		}

		if (!paused_)
			StartTimer();
	}

	// ========================================================
	// Pause
	// ========================================================

	void TogglePause()
	{
		if (FileCount() == 0)
			return;

		paused_ =
			!paused_;

		if (paused_)
			StopTimer();
		else
			StartTimer();

		UpdateUi();
	}

	// ========================================================
	// Timer
	// ========================================================

	void StartTimer()
	{
		if (
			FileCount() == 0 ||
			paused_)
		{
			return;
		}

		UINT interval =
			static_cast<UINT>(
				speedTenths_ * 100);

		if (interval < 100)
			interval = 100;

		SetTimer(
			hWnd_,
			TIMER_PLAY,
			interval,
			nullptr);
	}

	void StopTimer()
	{
		if (hWnd_)
		{
			KillTimer(
				hWnd_,
				TIMER_PLAY);
		}
	}

	void ChangeSpeed(
		int delta)
	{
		speedTenths_ =
			ClampValue(
				speedTenths_ + delta,
				1,
				50);

		if (speedSlider_)
		{
			SendMessageW(
				speedSlider_,
				TBM_SETPOS,
				TRUE,
				speedTenths_);
		}

		if (!paused_)
			StartTimer();

		UpdateUi();
	}

	// ========================================================
	// Save
	// ========================================================

	void SaveImage()
	{
		const std::wstring src =
			CurrentPath();

		if (src.empty())
			return;

		std::wstring saveDirCopy;
		std::wstring currentFolderCopy;

		{
			std::lock_guard<std::mutex> lock(
				saveDirMutex_);

			saveDirCopy =
				saveDir_;

			currentFolderCopy =
				currentFolder_;
		}

		if (saveDirCopy.empty())
		{
			MessageBoxW(
				hWnd_,
				L"请先设置保存目录！",
				L"警告",
				MB_ICONWARNING);

			return;
		}

		if (!CreateDirectoryRecursive(
			saveDirCopy))
		{
			MessageBoxW(
				hWnd_,
				L"无法创建保存目录。",
				L"错误",
				MB_ICONERROR);

			return;
		}

		std::wstring relativePath;

		if (recursiveScan_.load())
		{
			relativePath =
				GetRelativePath(
					currentFolderCopy,
					src);
		}
		else
		{
			relativePath =
				GetFileNameOnly(src);
		}

		if (relativePath.empty())
		{
			relativePath =
				GetFileNameOnly(src);
		}

		const std::wstring key =
			NormalizeRelativePath(
				relativePath);

		if (
			savedFiles_.find(key) !=
			savedFiles_.end())
		{
			SetWindowTextW(
				infoLabel_,
				L"当前图片已经保存过");

			if (!paused_)
				TogglePause();

			return;
		}

		const std::wstring dst =
			saveDirCopy +
			L"\\" +
			relativePath;

		const size_t slash =
			dst.find_last_of(
				L"\\/");

		if (
			slash !=
			std::wstring::npos)
		{
			const std::wstring parent =
				dst.substr(
					0,
					slash);

			if (!CreateDirectoryRecursive(
				parent))
			{
				MessageBoxW(
					hWnd_,
					L"无法创建保存图片的子目录。",
					L"错误",
					MB_ICONERROR);

				return;
			}
		}

		if (!CopyFileW(
			src.c_str(),
			dst.c_str(),
			TRUE))
		{
			const DWORD error =
				GetLastError();

			if (
				error == ERROR_FILE_EXISTS ||
				error == ERROR_ALREADY_EXISTS)
			{
				savedFiles_.insert(
					key);

				savedCount_ =
					savedFiles_.size();

				AppendSavedList(
					relativePath);

				SetWindowTextW(
					infoLabel_,
					L"目标目录已有该图片");

				if (!paused_)
					TogglePause();

				UpdateSavedLabel();
				UpdateUi();

				return;
			}

			MessageBoxW(
				hWnd_,
				L"保存失败，无法复制当前图片。",
				L"错误",
				MB_ICONERROR);

			return;
		}

		savedFiles_.insert(
			key);

		savedCount_ =
			savedFiles_.size();

		AppendSavedList(
			relativePath);

		if (!paused_)
			TogglePause();

		UpdateSavedLabel();
		UpdateUi();

		const std::wstring info =
			L"已保存: " +
			relativePath;

		SetWindowTextW(
			infoLabel_,
			info.c_str());
	}

	// ========================================================
	// Saved list
	// ========================================================

	void AppendSavedList(
		const std::wstring& relativePath)
	{
		std::wstring saveDirCopy;

		{
			std::lock_guard<std::mutex> lock(
				saveDirMutex_);

			saveDirCopy =
				saveDir_;
		}

		if (
			saveDirCopy.empty() ||
			relativePath.empty())
		{
			return;
		}

		const std::wstring listPath =
			saveDirCopy +
			L"\\saved_list.txt";

		HANDLE file =
			CreateFileW(
				listPath.c_str(),
				GENERIC_READ |
				GENERIC_WRITE,
				FILE_SHARE_READ,
				nullptr,
				OPEN_ALWAYS,
				FILE_ATTRIBUTE_NORMAL |
				FILE_FLAG_SEQUENTIAL_SCAN,
				nullptr);

		if (file == INVALID_HANDLE_VALUE)
			return;

		LARGE_INTEGER size;
		size.QuadPart = 0;

		GetFileSizeEx(
			file,
			&size);

		const bool needBom =
			size.QuadPart == 0;

		SetFilePointer(
			file,
			0,
			nullptr,
			FILE_END);

		if (needBom)
		{
			const unsigned char bom[3] =
			{
				0xEF,
				0xBB,
				0xBF
			};

			DWORD written = 0;

			WriteFile(
				file,
				bom,
				3,
				&written,
				nullptr);
		}

		const std::string line =
			WideToUtf8(
				relativePath +
				L"\r\n");

		if (!line.empty())
		{
			DWORD written = 0;

			WriteFile(
				file,
				line.data(),
				static_cast<DWORD>(
					line.size()),
				&written,
				nullptr);
		}

		CloseHandle(
			file);
	}

	bool LoadSavedListFile(
		const std::wstring& folder)
	{
		if (folder.empty())
			return false;

		const std::wstring listPath =
			folder +
			L"\\saved_list.txt";

		HANDLE file =
			CreateFileW(
				listPath.c_str(),
				GENERIC_READ,
				FILE_SHARE_READ |
				FILE_SHARE_WRITE |
				FILE_SHARE_DELETE,
				nullptr,
				OPEN_EXISTING,
				FILE_ATTRIBUTE_NORMAL |
				FILE_FLAG_SEQUENTIAL_SCAN,
				nullptr);

		if (file == INVALID_HANDLE_VALUE)
			return false;

		LARGE_INTEGER size;
		size.QuadPart = 0;

		if (!GetFileSizeEx(
			file,
			&size))
		{
			CloseHandle(
				file);

			return false;
		}

		if (
			size.QuadPart <= 3 ||
			size.QuadPart >
			64LL * 1024LL * 1024LL)
		{
			CloseHandle(
				file);

			return size.QuadPart == 3;
		}

		std::string content(
			static_cast<size_t>(
				size.QuadPart),
			'\0');

		DWORD totalRead = 0;

		while (
			totalRead <
			static_cast<DWORD>(
				content.size()))
		{
			DWORD toRead =
				static_cast<DWORD>(
					content.size() -
					totalRead);

			DWORD currentRead = 0;

			if (!ReadFile(
				file,
				&content[totalRead],
				toRead,
				&currentRead,
				nullptr))
			{
				CloseHandle(
					file);

				return false;
			}

			if (currentRead == 0)
				break;

			totalRead +=
				currentRead;
		}

		CloseHandle(
			file);

		if (totalRead == 0)
			return true;

		content.resize(
			totalRead);

		size_t offset = 0;

		if (
			content.size() >= 3 &&
			static_cast<unsigned char>(
				content[0]) == 0xEF &&
			static_cast<unsigned char>(
				content[1]) == 0xBB &&
			static_cast<unsigned char>(
				content[2]) == 0xBF)
		{
			offset = 3;
		}

		size_t lineStart =
			offset;

		for (
			size_t i = offset;
			i <= content.size();
			++i)
		{
			if (
				i != content.size() &&
				content[i] != '\n')
			{
				continue;
			}

			size_t lineEnd =
				i;

			if (
				lineEnd > lineStart &&
				content[lineEnd - 1] == '\r')
			{
				--lineEnd;
			}

			if (lineEnd > lineStart)
			{
				const std::string line =
					content.substr(
						lineStart,
						lineEnd -
						lineStart);

				const std::wstring relative =
					Utf8ToWide(
						line);

				if (!relative.empty())
				{
					savedFiles_.insert(
						NormalizeRelativePath(
							relative));
				}
			}

			lineStart =
				i + 1;
		}

		return true;
	}

	void LoadSavedState(
		const std::wstring& folder)
	{
		savedFiles_.clear();

		savedCount_ = 0;

		if (folder.empty())
			return;

		if (LoadSavedListFile(
			folder))
		{
			savedCount_ =
				savedFiles_.size();

			return;
		}

		std::vector<std::wstring>
			exclude;

		const std::vector<std::wstring> files =
			ScanImageFiles(
				folder,
				true,
				exclude);

		for (
			size_t i = 0;
			i < files.size();
			++i)
		{
			const std::wstring relative =
				GetRelativePath(
					folder,
					files[i]);

			if (relative.empty())
				continue;

			savedFiles_.insert(
				NormalizeRelativePath(
					relative));
		}

		savedCount_ =
			savedFiles_.size();
	}

	// ========================================================
	// UI update
	// ========================================================

	void UpdateSavedLabel()
	{
		if (!savedLabel_)
			return;

		wchar_t buffer[128];

		swprintf_s(
			buffer,
			_countof(buffer),
			L"已保存: %zu",
			savedCount_);

		SetWindowTextW(
			savedLabel_,
			buffer);
	}

	void UpdateSaveDirLabel()
	{
		if (!saveDirLabel_)
			return;

		std::wstring saveDirCopy;

		{
			std::lock_guard<std::mutex> lock(
				saveDirMutex_);

			saveDirCopy =
				saveDir_;
		}

		const std::wstring text =
			saveDirCopy.empty()
			? L"保存目录: 未设置"
			: L"保存目录: " +
			saveDirCopy;

		SetWindowTextW(
			saveDirLabel_,
			text.c_str());
	}

	void UpdateUi()
	{
		const size_t count =
			FileCount();

		if (count == 0)
		{
			SetWindowTextW(
				progressLabel_,
				L"0 / 0");

			UpdateSavedLabel();
			UpdateSaveDirLabel();

			return;
		}

		wchar_t buffer[256];

		swprintf_s(
			buffer,
			_countof(buffer),
			L"%zu / %zu",
			currentIndex_ + 1,
			count);

		SetWindowTextW(
			progressLabel_,
			buffer);

		UpdateSavedLabel();

		std::wstring info =
			std::to_wstring(
				currentIndex_ + 1) +
			L"/" +
			std::to_wstring(
				count);

		if (paused_)
			info +=
			L" (暂停)";

		if (decodeFailed_)
			info +=
			L" (加载失败)";

		if (
			currentDecodePending_.load())
		{
			info +=
				L" (解码中)";
		}

		if (
			consecutiveDecodeFailures_ >
			0)
		{
			info +=
				L" 连续失败:";

			info +=
				std::to_wstring(
					consecutiveDecodeFailures_);
		}

		SetWindowTextW(
			infoLabel_,
			info.c_str());

		const double seconds =
			speedTenths_ / 10.0;

		swprintf_s(
			buffer,
			_countof(buffer),
			L"间隔: %.1f 秒",
			seconds);

		SetWindowTextW(
			speedLabel_,
			buffer);

		UpdateSaveDirLabel();
	}

	void EnableNavigation(
		bool enable)
	{
		EnableWindow(
			prevBtn_,
			enable);

		EnableWindow(
			pauseBtn_,
			enable);

		EnableWindow(
			nextBtn_,
			enable);

		EnableWindow(
			saveBtn_,
			enable);

		EnableWindow(
			speedSlider_,
			enable);
	}

	// ========================================================
	// Pick Folder
	// ========================================================

	std::wstring PickFolder(
		const wchar_t* title)
	{
		IFileDialog* dialog =
			nullptr;

		HRESULT hr =
			CoCreateInstance(
				CLSID_FileOpenDialog,
				nullptr,
				CLSCTX_INPROC_SERVER,
				IID_PPV_ARGS(
					&dialog));

		if (FAILED(hr))
			return L"";

		DWORD options = 0;

		dialog->GetOptions(
			&options);

		dialog->SetOptions(
			options |
			FOS_PICKFOLDERS |
			FOS_FORCEFILESYSTEM);

		dialog->SetTitle(
			title);

		hr =
			dialog->Show(
				hWnd_);

		std::wstring result;

		if (SUCCEEDED(hr))
		{
			IShellItem* item =
				nullptr;

			if (
				SUCCEEDED(
					dialog->GetResult(
						&item)))
			{
				PWSTR path = nullptr;

				if (
					SUCCEEDED(
						item->GetDisplayName(
							SIGDN_FILESYSPATH,
							&path)) &&
					path)
				{
					result =
						path;

					CoTaskMemFree(
						path);
				}

				item->Release();
			}
		}

		dialog->Release();

		return result;
	}

	// ========================================================
	// Save directory
	// ========================================================

	void SelectSaveDir()
	{
		const std::wstring folder =
			PickFolder(
				L"选择保存图片的文件夹");

		if (folder.empty())
			return;

		std::wstring sourceFolder;

		{
			std::lock_guard<std::mutex> lock(
				saveDirMutex_);

			sourceFolder =
				currentFolder_;
		}

		if (
			!sourceFolder.empty() &&
			SameFolderPath(
				folder,
				sourceFolder))
		{
			MessageBoxW(
				hWnd_,
				L"保存目录不能与图片源目录相同。",
				L"警告",
				MB_ICONWARNING);

			return;
		}

		if (!CreateDirectoryRecursive(
			folder))
		{
			MessageBoxW(
				hWnd_,
				L"无法创建保存目录。",
				L"错误",
				MB_ICONERROR);

			return;
		}

		{
			std::lock_guard<std::mutex> lock(
				saveDirMutex_);

			saveDir_ =
				folder;
		}

		LoadSavedState(
			folder);

		UpdateSavedLabel();
		UpdateUi();
	}

	// ========================================================
	// Files
	// ========================================================

	size_t FileCount()
	{
		std::lock_guard<std::mutex> lock(
			filesMutex_);

		return files_.size();
	}

	std::wstring FileAt(
		size_t index)
	{
		std::lock_guard<std::mutex> lock(
			filesMutex_);

		if (index >= files_.size())
			return L"";

		return files_[index];
	}

	std::wstring CurrentPath()
	{
		return FileAt(
			currentIndex_);
	}

	// ========================================================
	// Pixel reset / clear
	// ========================================================

	void ResetPixelState()
	{
		lastPixelX_ = -1;
		lastPixelY_ = -1;

		lastPixelUpdateTick_ =
			0;

		fullPixelText_ =
			L"像素: 无";

		if (pixelInfoLabel_)
		{
			SetWindowTextW(
				pixelInfoLabel_,
				fullPixelText_.c_str());
		}

		UpdatePixelTooltip();
	}

	void ClearPixelRequest()
	{
		{
			std::lock_guard<std::mutex> lock(
				pixelMutex_);

			pixelPending_ =
				false;

			pixelPendingPath_.clear();
		}

		pixelCv_.notify_one();
	}

	// ========================================================
	// Workers start
	// ========================================================

	bool StartWorkers()
	{
		try
		{
			scanThreadExit_ =
				false;

			decodeThreadExit_ =
				false;

			pixelThreadExit_ =
				false;

			scanThread_ =
				std::thread(
					&ImageScreenerApp::ScanWorkerProc,
					this);

			decodeThread_ =
				std::thread(
					&ImageScreenerApp::DecodeWorkerProc,
					this);

			pixelThread_ =
				std::thread(
					&ImageScreenerApp::PixelWorkerProc,
					this);
		}
		catch (...)
		{
			shuttingDown_ =
				true;

			SignalWorkersToStop();

			if (scanThread_.joinable())
				scanThread_.join();

			if (decodeThread_.joinable())
				decodeThread_.join();

			if (pixelThread_.joinable())
				pixelThread_.join();

			return false;
		}

		return true;
	}

	void SignalWorkersToStop()
	{
		scanThreadExit_ =
			true;

		decodeThreadExit_ =
			true;

		pixelThreadExit_ =
			true;

		scanCv_.notify_all();
		decodeCv_.notify_all();
		pixelCv_.notify_all();
	}

	void StopWorkers()
	{
		SignalWorkersToStop();

		if (scanThread_.joinable())
			scanThread_.join();

		if (decodeThread_.joinable())
			decodeThread_.join();

		if (pixelThread_.joinable())
			pixelThread_.join();
	}

	// ========================================================
	// Decode Queue
	// ========================================================

	void ClearDecodeQueue()
	{
		std::lock_guard<std::mutex> lock(
			decodeMutex_);

		decodeQueue_.clear();
	}

	void ClearPreloadBitmaps()
	{
		std::lock_guard<std::mutex> lock(
			bitmapMutex_);

		DeleteBitmapData(
			preloadNextBitmap_);

		preloadNextPath_.clear();
	}

	void ClearBitmaps()
	{
		std::lock_guard<std::mutex> lock(
			bitmapMutex_);

		DeleteBitmapData(
			currentBitmap_);

		DeleteBitmapData(
			preloadNextBitmap_);

		preloadNextPath_.clear();
	}

	void QueueDecodeTask(
		const DecodeTask& task)
	{
		if (shuttingDown_)
			return;

		{
			std::lock_guard<std::mutex> lock(
				decodeMutex_);

			if (decodeThreadExit_)
				return;

			if (
				task.kind ==
				DecodeTaskKind::Current)
			{
				decodeQueue_.clear();

				decodeQueue_.push_front(
					task);
			}
			else
			{
				for (
					std::deque<DecodeTask>::iterator it =
					decodeQueue_.begin();
					it != decodeQueue_.end();)
				{
					if (
						it->kind ==
						DecodeTaskKind::PreloadNext)
					{
						it =
							decodeQueue_.erase(
								it);
					}
					else
					{
						++it;
					}
				}

				while (
					decodeQueue_.size() >= 2)
				{
					decodeQueue_.pop_back();
				}

				decodeQueue_.push_back(
					task);
			}
		}

		decodeCv_.notify_one();
	}

	// ========================================================
	// Scan worker
	// ========================================================

	void ScanWorkerProc()
	{
		for (;;)
		{
			std::wstring folder;

			unsigned long request =
				0;

			{
				std::unique_lock<std::mutex> lock(
					scanMutex_);

				scanCv_.wait(
					lock,
					[this]()
				{
					return
						scanThreadExit_.load() ||
						scanPending_;
				});

				if (scanThreadExit_)
					break;

				folder =
					scanFolder_;

				request =
					scanPendingRequestId_;

				scanPending_ =
					false;
			}

			const bool recursive =
				recursiveScan_.load();

			std::vector<std::wstring>
				excludeFolders;

			excludeFolders.push_back(
				folder +
				L"\\saved");

			std::wstring saveDirCopy;

			{
				std::lock_guard<std::mutex> lock(
					saveDirMutex_);

				saveDirCopy =
					saveDir_;
			}

			if (
				!saveDirCopy.empty() &&
				IsPathInsideFolder(
					saveDirCopy,
					folder))
			{
				excludeFolders.push_back(
					saveDirCopy);
			}

			std::vector<std::wstring>
				local =
				ScanImageFiles(
					folder,
					recursive,
					excludeFolders);

			if (
				scanThreadExit_ ||
				shuttingDown_)
			{
				continue;
			}

			if (
				request !=
				scanRequestId_.load())
			{
				continue;
			}

			{
				std::lock_guard<std::mutex> lock(
					filesMutex_);

				files_.swap(
					local);

				if (files_.empty())
				{
					currentIndex_ =
						0;
				}
				else if (
					currentIndex_ >=
					files_.size())
				{
					currentIndex_ =
						files_.size() - 1;
				}
			}

			if (
				!shuttingDown_ &&
				hWnd_)
			{
				PostMessageW(
					hWnd_,
					WM_APP_SCAN_DONE,
					static_cast<WPARAM>(
						request),
					0);
			}
		}
	}

	// ========================================================
	// Decode validation
	// ========================================================

	bool IsDecodeTaskValid(
		const DecodeTask& task) const
	{
		if (shuttingDown_)
			return false;

		if (
			task.kind ==
			DecodeTaskKind::Current)
		{
			return
				task.requestId ==
				decodeRequestId_.load();
		}

		return
			task.requestId ==
			preloadRequestId_.load();
	}

	// ========================================================
	// Decode worker
	// ========================================================

	void DecodeWorkerProc()
	{
		ImageDecoder decoder;

		if (!decoder.IsReady())
		{
			while (!decodeThreadExit_)
			{
				std::unique_lock<std::mutex> lock(
					decodeMutex_);

				decodeCv_.wait(
					lock,
					[this]()
				{
					return
						decodeThreadExit_.load();
				});
			}

			return;
		}

		for (;;)
		{
			DecodeTask task;

			{
				std::unique_lock<std::mutex> lock(
					decodeMutex_);

				decodeCv_.wait(
					lock,
					[this]()
				{
					return
						decodeThreadExit_.load() ||
						!decodeQueue_.empty();
				});

				if (decodeThreadExit_)
					break;

				task =
					decodeQueue_.front();

				decodeQueue_.pop_front();
			}

			if (!IsDecodeTaskValid(task))
				continue;

			BitmapData bitmap;

			const bool success =
				decoder.DecodeFit(
					task.path,
					task.width,
					task.height,
					bitmap);

			if (!IsDecodeTaskValid(task))
			{
				DeleteBitmapData(
					bitmap);

				continue;
			}

			if (
				task.kind ==
				DecodeTaskKind::Current)
			{
				bool applied =
					false;

				{
					std::lock_guard<std::mutex> lock(
						bitmapMutex_);

					if (IsDecodeTaskValid(task))
					{
						DeleteBitmapData(
							currentBitmap_);

						if (success)
						{
							MoveBitmapData(
								currentBitmap_,
								bitmap);

							decodeFailed_ =
								false;
						}
						else
						{
							decodeFailed_ =
								true;
						}

						applied =
							true;
					}
				}

				if (!applied)
				{
					DeleteBitmapData(
						bitmap);

					continue;
				}

				DeleteBitmapData(
					bitmap);

				if (
					!shuttingDown_ &&
					hWnd_)
				{
					PostMessageW(
						hWnd_,
						WM_APP_DECODE_DONE,
						static_cast<WPARAM>(
							task.requestId),
						0);
				}
			}
			else
			{
				std::lock_guard<std::mutex> lock(
					bitmapMutex_);

				if (!IsDecodeTaskValid(task))
				{
					DeleteBitmapData(
						bitmap);

					continue;
				}

				DeleteBitmapData(
					preloadNextBitmap_);

				preloadNextPath_.clear();

				if (success)
				{
					MoveBitmapData(
						preloadNextBitmap_,
						bitmap);

					preloadNextPath_ =
						task.path;
				}

				DeleteBitmapData(
					bitmap);
			}
		}
	}

	// ========================================================
	// Pixel worker
	// ========================================================

	void PixelWorkerProc()
	{
		PixelDecoder decoder;

		if (!decoder.IsReady())
		{
			while (!pixelThreadExit_)
			{
				std::unique_lock<std::mutex> lock(
					pixelMutex_);

				pixelCv_.wait(
					lock,
					[this]()
				{
					return
						pixelThreadExit_.load();
				});
			}

			return;
		}

		for (;;)
		{
			std::wstring path;

			int x = 0;
			int y = 0;

			unsigned long request =
				0;

			{
				std::unique_lock<std::mutex> lock(
					pixelMutex_);

				pixelCv_.wait(
					lock,
					[this]()
				{
					return
						pixelThreadExit_.load() ||
						pixelPending_;
				});

				if (pixelThreadExit_)
					break;

				path =
					pixelPendingPath_;

				x =
					pixelPendingX_;

				y =
					pixelPendingY_;

				request =
					pixelPendingRequestId_;

				pixelPending_ =
					false;
			}

			if (shuttingDown_)
				break;

			if (
				request !=
				pixelRequestId_.load())
			{
				continue;
			}

			PixelResult result;

			result.requestId =
				request;

			const bool success =
				decoder.GetPixel(
					path,
					x,
					y,
					result);

			if (
				!success ||
				request !=
				pixelRequestId_.load())
			{
				continue;
			}

			result.requestId =
				request;

			result.valid =
				true;

			{
				std::lock_guard<std::mutex> lock(
					pixelResultMutex_);

				pixelResult_ =
					result;
			}

			if (
				!shuttingDown_ &&
				hWnd_)
			{
				PostMessageW(
					hWnd_,
					WM_APP_PIXEL_DONE,
					static_cast<WPARAM>(
						request),
					0);
			}
		}
	}

private:

	// ========================================================
	// Window
	// ========================================================

	HINSTANCE hInst_;
	HWND hWnd_;

	HWND display_;

	HWND openBtn_;
	HWND saveDirBtn_;
	HWND prevBtn_;
	HWND pauseBtn_;
	HWND nextBtn_;
	HWND saveBtn_;

	HWND speedLabel_;
	HWND speedSlider_;

	HWND infoLabel_;
	HWND savedLabel_;
	HWND progressLabel_;
	HWND saveDirLabel_;
	HWND pixelInfoLabel_;

	// 修复过的成员
	HWND recursiveCheck_;
	HWND autoSkipCheck_;

	HWND pixelTooltip_;

	HFONT font_;
	HFONT boldFont_;

	// ========================================================
	// Files
	// ========================================================

	std::vector<std::wstring>
		files_;

	std::mutex
		filesMutex_;

	// ========================================================
	// Bitmap
	// ========================================================

	std::mutex
		bitmapMutex_;

	BitmapData
		currentBitmap_;

	BitmapData
		preloadNextBitmap_;

	std::wstring
		preloadNextPath_;

	std::atomic<bool>
		decodeFailed_{ false };

	// ========================================================
	// UI state
	// ========================================================

	size_t currentIndex_ =
		0;

	size_t savedCount_ =
		0;

	bool paused_ =
		false;

	int speedTenths_ =
		10;

	std::mutex
		saveDirMutex_;

	std::wstring
		currentFolder_;

	std::wstring
		saveDir_;

	std::unordered_set<std::wstring>
		savedFiles_;

	std::atomic<bool>
		recursiveScan_{ true };

	std::atomic<bool>
		autoSkipDecodeFailure_{ true };

	size_t
		consecutiveDecodeFailures_ =
		0;

	// ========================================================
	// Decode gate
	// ========================================================

	std::atomic<bool>
		currentDecodePending_{ false };

	// ========================================================
	// Pixel
	// ========================================================

	int lastPixelX_;
	int lastPixelY_;

	ULONGLONG
		lastPixelUpdateTick_;

	std::wstring
		fullPixelText_;

	// ========================================================
	// Scan thread
	// ========================================================

	std::thread
		scanThread_;

	std::mutex
		scanMutex_;

	std::condition_variable
		scanCv_;

	bool scanPending_;

	std::wstring
		scanFolder_;

	unsigned long
		scanPendingRequestId_ =
		0;

	std::atomic<bool>
		scanThreadExit_;

	// ========================================================
	// Decode thread
	// ========================================================

	std::thread
		decodeThread_;

	std::mutex
		decodeMutex_;

	std::condition_variable
		decodeCv_;

	std::deque<DecodeTask>
		decodeQueue_;

	std::atomic<bool>
		decodeThreadExit_;

	// ========================================================
	// Pixel thread
	// ========================================================

	std::thread
		pixelThread_;

	std::mutex
		pixelMutex_;

	std::condition_variable
		pixelCv_;

	bool pixelPending_;

	std::wstring
		pixelPendingPath_;

	int
		pixelPendingX_;

	int
		pixelPendingY_;

	unsigned long
		pixelPendingRequestId_;

	std::atomic<unsigned long>
		pixelRequestId_{ 0 };

	std::mutex
		pixelResultMutex_;

	PixelResult
		pixelResult_;

	std::atomic<bool>
		pixelThreadExit_;

	// ========================================================
	// Request IDs
	// ========================================================

	std::atomic<unsigned long>
		scanRequestId_{ 0 };

	std::atomic<unsigned long>
		decodeRequestId_{ 0 };

	std::atomic<unsigned long>
		preloadRequestId_{ 0 };

	std::atomic<bool>
		shuttingDown_{ false };

	// ========================================================
	// Display subclass
	// ========================================================

	WNDPROC
		oldDisplayProc_;
};

// ============================================================
// WinMain
// ============================================================

int APIENTRY wWinMain(
	HINSTANCE hInstance,
	HINSTANCE,
	LPWSTR,
	int nCmdShow)
{
	(void)nCmdShow;

	INITCOMMONCONTROLSEX icc;

	ZeroMemory(
		&icc,
		sizeof(icc));

	icc.dwSize =
		sizeof(icc);

	icc.dwICC =
		ICC_BAR_CLASSES;

	InitCommonControlsEx(
		&icc);

	HRESULT hr =
		CoInitializeEx(
			nullptr,
			COINIT_APARTMENTTHREADED);

	if (FAILED(hr))
		return 1;

	ImageScreenerApp app(
		hInstance);

	if (!app.Create())
	{
		CoUninitialize();

		return 1;
	}

	// ========================================================
	// 快捷键
	// ========================================================

	ACCEL accels[] =
	{
		{ FVIRTKEY, VK_LEFT,   ID_BTN_PREV },
		{ FVIRTKEY, VK_RIGHT,  ID_BTN_NEXT },
		{ FVIRTKEY, VK_RETURN, ID_BTN_PAUSE },
		{ FVIRTKEY, VK_SPACE,  ID_BTN_SAVE },
		{ FVIRTKEY, VK_UP,     ID_SPEED_UP },
		{ FVIRTKEY, VK_DOWN,   ID_SPEED_DOWN }
	};

	const int accelCount =
		static_cast<int>(
			sizeof(accels) /
			sizeof(accels[0]));

	HACCEL hAccel =
		CreateAcceleratorTableW(
			accels,
			accelCount);

	MSG msg;

	ZeroMemory(
		&msg,
		sizeof(msg));

	while (
		GetMessageW(
			&msg,
			nullptr,
			0,
			0) > 0)
	{
		if (
			!TranslateAcceleratorW(
				app.GetHwnd(),
				hAccel,
				&msg))
		{
			TranslateMessage(
				&msg);

			DispatchMessageW(
				&msg);
		}
	}

	if (hAccel)
	{
		DestroyAcceleratorTable(
			hAccel);
	}

	CoUninitialize();

	return 0;
}
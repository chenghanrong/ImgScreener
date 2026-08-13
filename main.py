import os
import tkinter as tk
from tkinter import filedialog, messagebox, ttk
from PIL import Image, ImageTk
import shutil

class ImageScreener:
    def __init__(self, root):
        self.root = root
        self.root.title("AI视觉图片筛查工具")
        # ----- 修改：窗口尺寸加大 -----
        self.root.geometry("1200x900")   # 从850x750改为1200x900

        # 状态变量
        self.image_files = []
        self.current_index = 0
        self.paused = False
        self.speed = 1.0
        self.save_dir = None
        self.timer_id = None
        self.current_image = None
        self.saved_count = 0

        # 创建界面
        self.create_widgets()

        # 绑定全局快捷键
        self.root.bind_all('<space>', self.save_image)
        self.root.bind_all('<Return>', self.toggle_pause)
        self.root.bind_all('<Left>', self.prev_image)
        self.root.bind_all('<Right>', self.next_image)
        self.root.bind_all('<Up>', self.increase_speed)
        self.root.bind_all('<Down>', self.decrease_speed)

        # 初始状态
        self.info_label.config(text="请点击「打开文件夹」加载图片")
        self.progress_label.config(text="0 / 0")
        self.saved_label.config(text="已保存: 0")

    def create_widgets(self):
        # 图片显示区域
        self.display_frame = tk.Frame(self.root, bg='#f0f0f0', bd=2, relief='sunken')
        # ----- 修改：减小边距，让显示区域更大 -----
        self.display_frame.pack(fill='both', expand=True, padx=5, pady=5)  # 从10减到5

        self.canvas = tk.Canvas(self.display_frame, bg='gray')
        self.canvas.pack(fill='both', expand=True)

        # 控制区域（上）
        control_frame = tk.Frame(self.root)
        control_frame.pack(fill='x', padx=10, pady=5)

        # 速度控制
        speed_frame = tk.Frame(control_frame)
        speed_frame.pack(side='left')
        tk.Label(speed_frame, text="间隔 (秒):").pack(side='left')
        self.speed_var = tk.DoubleVar(value=self.speed)
        self.speed_scale = tk.Scale(speed_frame, from_=0.1, to=5.0, resolution=0.1,
                                    orient='horizontal', variable=self.speed_var,
                                    length=150, command=self.on_speed_change)
        self.speed_scale.pack(side='left', padx=5)

        # 打开文件夹按钮
        self.open_btn = tk.Button(control_frame, text="打开文件夹", command=self.select_folder)
        self.open_btn.pack(side='left', padx=10)

        # 状态信息（当前进度）
        self.info_label = tk.Label(control_frame, text="未加载图片")
        self.info_label.pack(side='left', padx=10)

        # 保存计数
        self.saved_label = tk.Label(control_frame, text="已保存: 0")
        self.saved_label.pack(side='left', padx=10)

        # 保存目录
        dir_frame = tk.Frame(control_frame)
        dir_frame.pack(side='right')
        self.dir_label = tk.Label(dir_frame, text="保存目录: 未设置", anchor='w', width=20)
        self.dir_label.pack(side='left')
        tk.Button(dir_frame, text="选择目录", command=self.select_save_dir).pack(side='left', padx=5)

        # 进度文本
        progress_frame = tk.Frame(self.root)
        progress_frame.pack(fill='x', padx=10, pady=2)
        self.progress_label = tk.Label(progress_frame, text="0 / 0", font=('Arial', 12))
        self.progress_label.pack()

        # 虚拟按钮区域
        btn_frame = tk.Frame(self.root)
        btn_frame.pack(fill='x', padx=10, pady=5)

        self.prev_btn = tk.Button(btn_frame, text="◀ 上一张", command=self.prev_image, width=10)
        self.prev_btn.pack(side='left', padx=5)

        self.pause_btn = tk.Button(btn_frame, text="⏸ 暂停/继续", command=self.toggle_pause, width=12)
        self.pause_btn.pack(side='left', padx=5)

        self.next_btn = tk.Button(btn_frame, text="下一张 ▶", command=self.next_image, width=10)
        self.next_btn.pack(side='left', padx=5)

        self.save_btn = tk.Button(btn_frame, text="💾 保存", command=self.save_image, width=10)
        self.save_btn.pack(side='left', padx=5)

        # 底部提示
        tip_frame = tk.Frame(self.root)
        tip_frame.pack(fill='x', padx=10, pady=5)
        tk.Label(tip_frame,
                 text="快捷键: [空格] 保存并暂停  [回车] 暂停/继续  [←][→] 上一张/下一张  [↑][↓] 加快/减慢速度").pack()

        self.set_buttons_state(tk.DISABLED)

    def set_buttons_state(self, state):
        self.prev_btn.config(state=state)
        self.pause_btn.config(state=state)
        self.next_btn.config(state=state)
        self.save_btn.config(state=state)

    def increase_speed(self, event=None):
        if not self.image_files:
            return
        new_speed = min(5.0, self.speed_var.get() + 0.1)
        self.speed_var.set(new_speed)
        self.on_speed_change()

    def decrease_speed(self, event=None):
        if not self.image_files:
            return
        new_speed = max(0.1, self.speed_var.get() - 0.1)
        self.speed_var.set(new_speed)
        self.on_speed_change()

    def select_folder(self):
        folder = filedialog.askdirectory(title="选择包含图片的文件夹")
        if not folder:
            return

        extensions = ('.jpg', '.jpeg', '.png', '.bmp', '.gif', '.tiff')
        self.image_files = []
        for f in os.listdir(folder):
            if f.lower().endswith(extensions):
                self.image_files.append(os.path.join(folder, f))
        self.image_files.sort()

        if not self.image_files:
            messagebox.showerror("错误", "所选文件夹中没有支持的图片文件！")
            return

        self.save_dir = os.path.join(folder, "saved")
        if not os.path.exists(self.save_dir):
            os.makedirs(self.save_dir)
        self.dir_label.config(text=f"保存目录: {self.save_dir}")

        self.current_index = 0
        self.saved_count = 0
        self.saved_label.config(text="已保存: 0")
        self.paused = False
        self.progress_label.config(text=f"1 / {len(self.image_files)}")

        self.set_buttons_state(tk.NORMAL)
        self.show_image()
        self.start_auto_play()

    def select_save_dir(self):
        dir_path = filedialog.askdirectory(title="选择保存图片的文件夹")
        if dir_path:
            self.save_dir = dir_path
            self.dir_label.config(text=f"保存目录: {dir_path}")

    def show_image(self):
        if not self.image_files:
            return
        file_path = self.image_files[self.current_index]
        try:
            self.root.title(f"AI视觉图片筛查工具 - {os.path.basename(file_path)}")
            self.info_label.config(text=f"{self.current_index+1}/{len(self.image_files)}")
            self.progress_label.config(text=f"{self.current_index+1} / {len(self.image_files)}")

            pil_img = Image.open(file_path)
            self.canvas.update_idletasks()
            canvas_width = self.canvas.winfo_width()
            canvas_height = self.canvas.winfo_height()
            if canvas_width <= 1:
                canvas_width = 800
                canvas_height = 600

            # 等比例缩放，保持图片不变形
            img_width, img_height = pil_img.size
            ratio = min(canvas_width / img_width, canvas_height / img_height, 1.0)
            new_width = int(img_width * ratio)
            new_height = int(img_height * ratio)

            if ratio < 1.0:
                pil_img = pil_img.resize((new_width, new_height), Image.LANCZOS)

            self.current_image = ImageTk.PhotoImage(pil_img)
            self.canvas.delete("image")
            self.canvas.create_image(
                (canvas_width - new_width) // 2,
                (canvas_height - new_height) // 2,
                anchor='nw',
                image=self.current_image,
                tags="image"
            )
        except Exception as e:
            messagebox.showerror("错误", f"无法加载图片: {file_path}\n{e}")

    def start_auto_play(self):
        self.schedule_next()

    def schedule_next(self):
        if self.timer_id:
            self.root.after_cancel(self.timer_id)
        if not self.paused:
            self.timer_id = self.root.after(int(self.speed * 1000), self.next_image)

    def next_image(self, event=None):
        if not self.image_files:
            return
        self.current_index = (self.current_index + 1) % len(self.image_files)
        self.show_image()
        if not self.paused:
            self.schedule_next()

    def prev_image(self, event=None):
        if not self.image_files:
            return
        self.current_index = (self.current_index - 1) % len(self.image_files)
        self.show_image()
        if not self.paused:
            self.schedule_next()

    def on_speed_change(self, event=None):
        self.speed = self.speed_var.get()
        if not self.paused:
            self.schedule_next()

    def toggle_pause(self, event=None):
        if not self.image_files:
            return
        self.paused = not self.paused
        if self.paused:
            self.info_label.config(text=f"{self.current_index+1}/{len(self.image_files)} (暂停)")
            if self.timer_id:
                self.root.after_cancel(self.timer_id)
                self.timer_id = None
        else:
            self.info_label.config(text=f"{self.current_index+1}/{len(self.image_files)}")
            self.schedule_next()

    def save_image(self, event=None):
        if not self.image_files:
            return
        if not self.save_dir:
            messagebox.showwarning("警告", "请先设置保存目录！")
            return

        src = self.image_files[self.current_index]
        dst = os.path.join(self.save_dir, os.path.basename(src))
        try:
            shutil.copy2(src, dst)
            self.saved_count += 1
            self.saved_label.config(text=f"已保存: {self.saved_count}")

            record_file = os.path.join(self.save_dir, "saved_list.txt")
            with open(record_file, 'a', encoding='utf-8') as f:
                f.write(os.path.basename(src) + '\n')

            if not self.paused:
                self.toggle_pause()

            self.info_label.config(text=f"已保存: {os.path.basename(src)}")
            self.root.after(2000, lambda: self.info_label.config(
                text=f"{self.current_index+1}/{len(self.image_files)}" + (" (暂停)" if self.paused else "")
            ))
        except Exception as e:
            messagebox.showerror("错误", f"保存失败: {e}")


if __name__ == "__main__":
    root = tk.Tk()
    app = ImageScreener(root)
    root.mainloop()
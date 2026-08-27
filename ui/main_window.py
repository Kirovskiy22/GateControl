import customtkinter as ctk

from controllers.gate_controller import GateController
from ui.theme import COLORS, WINDOW_HEIGHT, WINDOW_WIDTH


class MainWindow(ctk.CTk):
    def __init__(self):
        super().__init__()

        self.title("Gate Control v1.0")
        self.geometry(f"{WINDOW_WIDTH}x{WINDOW_HEIGHT}")
        self.configure(fg_color=COLORS["background"])

        self.build_ui()
        self.controller = GateController(self)

        self.open_btn.configure(command=self.controller.open_gate)
        self.close_btn.configure(command=self.controller.close_gate)
        self.refresh_btn.configure(command=self.controller.refresh)
        self.protocol("WM_DELETE_WINDOW", self.on_close)

    def build_ui(self):
        header = ctk.CTkFrame(
            self,
            corner_radius=15,
            fg_color=COLORS["frame"],
            height=70,
        )
        header.pack(fill="x", padx=15, pady=15)

        title = ctk.CTkLabel(
            header,
            text="🛡 Gate Control v1.0",
            font=("Segoe UI", 24, "bold"),
        )
        title.pack(side="left", padx=20, pady=15)

        self.connection = ctk.CTkLabel(
            header,
            text="🟢 Connected",
            text_color=COLORS["green"],
            font=("Segoe UI", 18),
        )
        self.connection.pack(side="right", padx=20)

        buttons = ctk.CTkFrame(self, fg_color="transparent")
        buttons.pack(pady=20)

        self.open_btn = ctk.CTkButton(
            buttons,
            width=250,
            height=60,
            text="🟢 ПОДНЯТЬ",
            font=("Segoe UI", 18, "bold"),
            fg_color=COLORS["green"],
            text_color="black",
        )
        self.open_btn.pack(pady=12)

        self.close_btn = ctk.CTkButton(
            buttons,
            width=250,
            height=60,
            text="🔴 ОПУСТИТЬ",
            font=("Segoe UI", 18, "bold"),
            fg_color=COLORS["red"],
        )
        self.close_btn.pack(pady=12)

        self.refresh_btn = ctk.CTkButton(
            buttons,
            width=250,
            height=50,
            text="🔄 ОБНОВИТЬ",
            font=("Segoe UI", 16),
        )
        self.refresh_btn.pack(pady=12)

        status = ctk.CTkFrame(
            self,
            corner_radius=15,
            fg_color=COLORS["frame"],
        )
        status.pack(fill="x", padx=15, pady=10)

        self.status_label = ctk.CTkLabel(
            status,
            text="⚪ Ожидание команды",
            font=("Segoe UI", 20, "bold"),
        )
        self.status_label.pack(pady=20)

        log_frame = ctk.CTkFrame(
            self,
            corner_radius=15,
            fg_color=COLORS["frame"],
        )
        log_frame.pack(fill="both", expand=True, padx=15, pady=15)

        log_title = ctk.CTkLabel(
            log_frame,
            text="Журнал",
            font=("Segoe UI", 18, "bold"),
        )
        log_title.pack(anchor="w", padx=15, pady=10)

        self.log = ctk.CTkTextbox(
            log_frame,
            font=("Consolas", 14),
        )
        self.log.pack(fill="both", expand=True, padx=15, pady=10)
        self.log.insert("end", "Gate Control запущен...\n")

    def on_close(self) -> None:
        self.controller.shutdown()
        self.destroy()

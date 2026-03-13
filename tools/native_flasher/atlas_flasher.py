import contextlib
import io
import queue
import sys
import threading
from pathlib import Path
import tkinter as tk
from tkinter import ttk, messagebox

import esptool
import serial.tools.list_ports


APP_TITLE = "ATLAS del Aire Flasher"
FIRMWARE_NAME = "atlas-del-aire-latest.bin"
FLASH_BAUDRATE = "115200"
FLASH_ADDRESS = "0x00000"


def app_base_dir() -> Path:
    if getattr(sys, "frozen", False):
        return Path(sys.executable).resolve().parent
    return Path(__file__).resolve().parents[2]


def resolve_firmware_path() -> Path | None:
    base = app_base_dir()
    candidates = [
        base / FIRMWARE_NAME,
        base / "firmware" / "releases" / FIRMWARE_NAME,
    ]
    for candidate in candidates:
        if candidate.exists():
            return candidate
    return None


def list_serial_ports():
    ports = []
    for port in serial.tools.list_ports.comports():
        label = f"{port.device} - {port.description}"
        ports.append((port.device, label))
    return ports


class QueueWriter(io.TextIOBase):
    def __init__(self, sink_queue: queue.Queue):
        self.sink_queue = sink_queue

    def write(self, data):
        if data:
            self.sink_queue.put(("log", data))
        return len(data)

    def flush(self):
        return None


class FlasherApp:
    def __init__(self, root: tk.Tk):
        self.root = root
        self.root.title(APP_TITLE)
        self.root.geometry("720x520")
        self.root.minsize(680, 480)

        self.events = queue.Queue()
        self.port_map = {}
        self.selected_port = tk.StringVar()
        self.status_text = tk.StringVar(value="Comprobando firmware...")
        self.port_status_text = tk.StringVar(value="Ningun puerto seleccionado.")
        self.progress_text = tk.StringVar(value="Esperando")
        self.is_flashing = False
        self.firmware_path = resolve_firmware_path()

        self.build_ui()
        self.refresh_ports()
        self.refresh_firmware_status()
        self.process_events()

    def build_ui(self):
        self.root.configure(padx=20, pady=20)

        title = ttk.Label(self.root, text="Actualizar panel", font=("Segoe UI", 22, "bold"))
        title.pack(anchor="w")

        subtitle = ttk.Label(
            self.root,
            text="Conecta el panel, elige el puerto y pulsa actualizar.",
            font=("Segoe UI", 10),
        )
        subtitle.pack(anchor="w", pady=(4, 18))

        status_frame = ttk.LabelFrame(self.root, text="Firmware")
        status_frame.pack(fill="x", pady=(0, 12))
        ttk.Label(status_frame, textvariable=self.status_text, wraplength=640).pack(anchor="w", padx=12, pady=12)

        port_frame = ttk.LabelFrame(self.root, text="Puerto")
        port_frame.pack(fill="x", pady=(0, 12))

        port_row = ttk.Frame(port_frame)
        port_row.pack(fill="x", padx=12, pady=(12, 6))

        self.port_combo = ttk.Combobox(
            port_row,
            textvariable=self.selected_port,
            state="readonly",
            width=58,
        )
        self.port_combo.pack(side="left", fill="x", expand=True)

        self.refresh_button = ttk.Button(port_row, text="Refrescar", command=self.refresh_ports)
        self.refresh_button.pack(side="left", padx=(10, 0))

        ttk.Label(port_frame, textvariable=self.port_status_text).pack(anchor="w", padx=12, pady=(0, 12))

        action_frame = ttk.Frame(self.root)
        action_frame.pack(fill="x", pady=(0, 16))

        self.flash_button = ttk.Button(action_frame, text="Actualizar panel", command=self.start_flash)
        self.flash_button.pack(side="left")

        self.progress_bar = ttk.Progressbar(self.root, mode="determinate", maximum=100)
        self.progress_bar.pack(fill="x")
        ttk.Label(self.root, textvariable=self.progress_text).pack(anchor="w", pady=(6, 14))

        log_frame = ttk.LabelFrame(self.root, text="Registro")
        log_frame.pack(fill="both", expand=True)

        self.log_box = tk.Text(log_frame, height=16, wrap="word", state="disabled")
        self.log_box.pack(fill="both", expand=True, padx=12, pady=12)

        self.port_combo.bind("<<ComboboxSelected>>", self.on_port_selected)

    def refresh_firmware_status(self):
        self.firmware_path = resolve_firmware_path()
        if self.firmware_path is None:
            self.status_text.set(
                f"Falta {FIRMWARE_NAME}. Colocalo junto al ejecutable o en firmware/releases/."
            )
        else:
            size_kb = self.firmware_path.stat().st_size / 1024
            self.status_text.set(f"{self.firmware_path.name} listo ({size_kb:.1f} KB)")
        self.update_controls()

    def refresh_ports(self):
        ports = list_serial_ports()
        self.port_map = {label: device for device, label in ports}
        labels = list(self.port_map.keys())
        self.port_combo["values"] = labels

        if labels:
            current = self.selected_port.get()
            if current not in self.port_map:
                self.selected_port.set(labels[0])
            self.on_port_selected()
        else:
            self.selected_port.set("")
            self.port_status_text.set("No se ha detectado ningun puerto serie.")
        self.update_controls()

    def on_port_selected(self, _event=None):
        label = self.selected_port.get()
        device = self.port_map.get(label)
        if device:
            self.port_status_text.set(f"Puerto seleccionado: {device}")
        else:
            self.port_status_text.set("Ningun puerto seleccionado.")
        self.update_controls()

    def update_controls(self):
        can_flash = (
            not self.is_flashing
            and self.firmware_path is not None
            and bool(self.port_map.get(self.selected_port.get()))
        )
        self.flash_button.config(state=("normal" if can_flash else "disabled"))
        self.refresh_button.config(state=("disabled" if self.is_flashing else "normal"))
        self.port_combo.config(state=("disabled" if self.is_flashing else "readonly"))

    def append_log(self, message: str):
        self.log_box.configure(state="normal")
        self.log_box.insert("end", message)
        self.log_box.see("end")
        self.log_box.configure(state="disabled")

    def set_progress(self, value: int, label: str):
        self.progress_bar["value"] = value
        self.progress_text.set(label)

    def start_flash(self):
        if self.firmware_path is None:
            messagebox.showerror(APP_TITLE, f"No se encuentra {FIRMWARE_NAME}.")
            return

        port = self.port_map.get(self.selected_port.get())
        if not port:
            messagebox.showerror(APP_TITLE, "Selecciona un puerto valido.")
            return

        self.is_flashing = True
        self.log_box.configure(state="normal")
        self.log_box.delete("1.0", "end")
        self.log_box.configure(state="disabled")
        self.set_progress(0, "Preparando")
        self.update_controls()

        worker = threading.Thread(target=self.flash_worker, args=(port, self.firmware_path), daemon=True)
        worker.start()

    def flash_worker(self, port: str, firmware_path: Path):
        writer = QueueWriter(self.events)
        argv_write = [
            "--port",
            port,
            "--baud",
            FLASH_BAUDRATE,
            "--no-stub",
            "write_flash",
            FLASH_ADDRESS,
            str(firmware_path),
        ]

        try:
            self.events.put(("progress", 5, "Preparando"))
            self.events.put(("log", f"Puerto: {port}\n"))
            self.events.put(("log", f"Firmware: {firmware_path}\n\n"))
            self.events.put(("progress", 25, "Escribiendo firmware"))

            with contextlib.redirect_stdout(writer), contextlib.redirect_stderr(writer):
                esptool.main(argv_write)

            self.events.put(("progress", 100, "Completado"))
            self.events.put(("done", True, "Actualizacion completada correctamente."))
        except SystemExit as exc:
            code = exc.code if isinstance(exc.code, int) else 1
            if code == 0:
                self.events.put(("progress", 100, "Completado"))
                self.events.put(("done", True, "Actualizacion completada correctamente."))
            else:
                self.events.put(("done", False, f"esptool ha terminado con codigo {code}."))
        except Exception as exc:
            self.events.put(("done", False, str(exc)))

    def process_events(self):
        try:
            while True:
                item = self.events.get_nowait()
                kind = item[0]
                if kind == "log":
                    self.append_log(item[1])
                elif kind == "progress":
                    _, value, label = item
                    self.set_progress(value, label)
                elif kind == "done":
                    _, ok, message = item
                    self.is_flashing = False
                    self.update_controls()
                    if ok:
                        messagebox.showinfo(APP_TITLE, message)
                    else:
                        self.set_progress(0, "Error")
                        messagebox.showerror(APP_TITLE, message)
        except queue.Empty:
            pass

        self.root.after(100, self.process_events)


def main():
    root = tk.Tk()
    style = ttk.Style(root)
    if "vista" in style.theme_names():
        style.theme_use("vista")
    app = FlasherApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()

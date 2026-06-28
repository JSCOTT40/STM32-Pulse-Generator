import sys
from PyQt5.QtWidgets import (
    QApplication, QWidget, QVBoxLayout, QHBoxLayout, QLabel, QSpinBox,
    QPushButton, QGroupBox, QGridLayout, QComboBox, QTextEdit
)
from PyQt5.QtGui import QPalette, QColor, QTextCursor, QIcon
from PyQt5.QtCore import Qt, QTimer
from matplotlib.backends.backend_qt5agg import FigureCanvasQTAgg as FigureCanvas
import matplotlib.pyplot as plt
import serial
import time
import threading
from datetime import datetime

UNITS = {"µs": 1, "ms": 1000, "s": 1_000_000}

class PulseGUI(QWidget):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("Pulse Generator")
        self.resize(1000, 800)
        self.serial_port = None
        self.running = False
        self.serial_thread = None
        self.ignore_updates = False
        
        self.missed_trigger = False  # Flag to track missed trigger state
        self.flash_state = False     # For flashing icon toggle
        
        self.initUI()
        self.apply_dark_theme()
        self.connect_serial()

    def initUI(self):
        """Setup all UI elements and layout."""
        main_layout = QVBoxLayout()
        self.inputs = {}

        tab_layout = QVBoxLayout()

        # Pulse Configuration Grid
        grid_box = QGroupBox("Pulse Configuration")
        grid = QGridLayout()
        grid.addWidget(QLabel("Channel"), 0, 0)
        grid.addWidget(QLabel("Width"), 0, 1)
        grid.addWidget(QLabel("Unit"), 0, 2)
        grid.addWidget(QLabel("Delay"), 0, 3)
        grid.addWidget(QLabel("Unit"), 0, 4)

        self.channel_colors = {
            1: '#FF0000', 2: '#00FF00', 
            3: '#0000FF', 4: '#FF00FF'
        }

        default_values = [
            (100, 0), (150, 200), 
            (200, 400), (250, 600)
        ]

        for i in range(1, 5):
            label = QLabel(f"Channel {i}")
            width = QSpinBox()
            width.setRange(0, 10_000_000)
            width.setValue(default_values[i-1][0])
            delay = QSpinBox()
            delay.setRange(0, 10_000_000)
            delay.setValue(default_values[i-1][1])
            
            width.valueChanged.connect(lambda val, idx=i: self.on_width_changed(val, idx))
            delay.valueChanged.connect(lambda val, idx=i: self.on_delay_changed(val, idx))
            
            width_unit = QComboBox()
            width_unit.addItems(UNITS.keys())
            width_unit.setCurrentText("µs")
            delay_unit = QComboBox()
            delay_unit.addItems(UNITS.keys())
            delay_unit.setCurrentText("µs")

            grid.addWidget(label, i, 0)
            grid.addWidget(width, i, 1)
            grid.addWidget(width_unit, i, 2)
            grid.addWidget(delay, i, 3)
            grid.addWidget(delay_unit, i, 4)

            self.inputs[f"CH{i}_width"] = width
            self.inputs[f"CH{i}_width_unit"] = width_unit
            self.inputs[f"CH{i}_delay"] = delay
            self.inputs[f"CH{i}_delay_unit"] = delay_unit

        grid_box.setLayout(grid)
        tab_layout.addWidget(grid_box)

        # Phase Delay Controls
        phase_box = QGroupBox("Phase Delay Configuration")
        phase_layout = QHBoxLayout()
        phase_layout.addWidget(QLabel("Phase Delay:"))
        self.phase_delay = QSpinBox()
        self.phase_delay.setRange(1, 10_000_000)
        self.phase_delay.setValue(1000)
        self.phase_unit = QComboBox()
        self.phase_unit.addItems(UNITS.keys())
        self.phase_unit.setCurrentText("µs")
        phase_layout.addWidget(self.phase_delay)
        phase_layout.addWidget(self.phase_unit)
        phase_box.setLayout(phase_layout)
        tab_layout.addWidget(phase_box)

        # Matplotlib Plot Area
        self.figure, self.ax = plt.subplots()
        self.figure.patch.set_facecolor("#1e1e1e")
        self.ax.set_facecolor("#1e1e1e")
        self.ax.tick_params(colors='white', labelcolor='white')
        self.canvas = FigureCanvas(self.figure)
        tab_layout.addWidget(self.canvas)

        # Status label and missed trigger icon container
        status_layout = QHBoxLayout()
        self.status_label = QLabel("Status: Disconnected")
        self.status_label.setStyleSheet("color: #FF0000; font-weight: bold;")
        status_layout.addWidget(self.status_label)

        # Missed Trigger blinking icon (red circle)
        self.missed_trigger_icon = QLabel()
        self.missed_trigger_icon.setFixedSize(20, 20)
        self.missed_trigger_icon.setStyleSheet("background-color: transparent; border-radius: 10px;")
        self.missed_trigger_icon.setToolTip("Missed Trigger Detected")
        status_layout.addWidget(self.missed_trigger_icon, alignment=Qt.AlignRight)
        tab_layout.addLayout(status_layout)

        # Buttons layout
        btn_layout = QHBoxLayout()
        self.send_btn = QPushButton("Send Config")
        self.start_btn = QPushButton("Start Now")
        self.arm_btn = QPushButton("Arm External Trigger")
        self.stop_btn = QPushButton("Stop Sequence")
        self.plot_btn = QPushButton("Plot")
        btn_layout.addWidget(self.send_btn)
        btn_layout.addWidget(self.start_btn)
        btn_layout.addWidget(self.arm_btn)
        btn_layout.addWidget(self.stop_btn)
        btn_layout.addWidget(self.plot_btn)
        tab_layout.addLayout(btn_layout)

        # Repetition rate label (Hz)
        self.repetition_label = QLabel("Repetition Rate: 0.0 Hz")
        self.repetition_label.setStyleSheet("color: #F0F0F0; font-weight: bold;")
        tab_layout.addWidget(self.repetition_label)

        # Console output box
        console_box = QGroupBox("Console Output")
        console_layout = QVBoxLayout()
        self.console_output = QTextEdit()
        self.console_output.setReadOnly(True)
        self.console_output.setStyleSheet("""
            QTextEdit {
                background-color: #1E1E1E;
                color: #DDDDDD;
                border: 1px solid #444;
                font-family: Consolas, Courier, monospace;
            }
        """)
        console_layout.addWidget(self.console_output)
        console_box.setLayout(console_layout)
        tab_layout.addWidget(console_box)

        main_layout.addLayout(tab_layout)
        self.setLayout(main_layout)

        # Connect buttons to methods
        self.send_btn.clicked.connect(self.send_config)
        self.start_btn.clicked.connect(self.start_pulse)
        self.arm_btn.clicked.connect(self.arm_external_trigger)
        self.stop_btn.clicked.connect(self.stop_sequence)
        self.plot_btn.clicked.connect(self.plot_pulses)

        # Timer to flash the missed trigger icon every 500 ms
        self.flash_timer = QTimer()
        self.flash_timer.setInterval(500)
        self.flash_timer.timeout.connect(self.toggle_missed_trigger_icon)
        self.flash_timer.start()

        # Ensure buttons always enabled initially
        self.update_button_states()

    def toggle_missed_trigger_icon(self):
        """Toggle visibility of missed trigger icon for blinking effect."""
        if self.missed_trigger:
            self.flash_state = not self.flash_state
            if self.flash_state:
                # Show red circle
                self.missed_trigger_icon.setStyleSheet(
                    "background-color: red; border-radius: 10px;"
                )
            else:
                # Hide (transparent)
                self.missed_trigger_icon.setStyleSheet(
                    "background-color: transparent; border-radius: 10px;"
                )
        else:
            # Always hide if no missed trigger
            self.missed_trigger_icon.setStyleSheet(
                "background-color: transparent; border-radius: 10px;"
            )

    def connect_serial(self):
        """Attempt to open serial port and start monitoring thread."""
        if self.serial_port and self.serial_port.is_open:
            return True
        try:
            self.serial_port = serial.Serial('COM4', 115200, timeout=1)
            time.sleep(2)  # Allow time for connection
            self.status_label.setText("Status: Connected")
            self.status_label.setStyleSheet("color: #00FF00; font-weight: bold;")
            self.log_message("Serial connection established", "COM")

            self.serial_thread = threading.Thread(target=self.monitor_serial)
            self.serial_thread.daemon = True
            self.serial_thread.start()
            return True
        except Exception as e:
            error_msg = f"Connection Failed ({str(e)})"
            self.status_label.setText(f"Status: {error_msg}")
            self.status_label.setStyleSheet("color: #FF0000; font-weight: bold;")
            self.log_message(error_msg, "ERROR")
            return False

    def monitor_serial(self):
        """Continuously monitor serial for incoming messages."""
        while self.serial_port and self.serial_port.is_open:
            try:
                if self.serial_port.in_waiting:
                    response = self.serial_port.readline().decode(errors='ignore').strip()
                    if response:
                        self.log_message(response, "STM32")
                        # Detect missed trigger messages
                        if "MISSED_TRIGGER" in response:
                            self.missed_trigger = True
                        # Clear missed trigger on STOP or START
                        if "STOPPED" in response or "STARTED" in response:
                            self.missed_trigger = False
                        self.update_button_states()
                time.sleep(0.01)
            except Exception as e:
                self.log_message(f"Serial error: {str(e)}", "ERROR")
                break

    def send_config(self):
        """Send current pulse configuration over serial."""
        if not self.ensure_serial():
            return
        pulses = self.get_channel_settings()
        phase_delay = self.get_phase_delay_us()

        cmd = "SET:" + ",".join(
            f"W{ch}={int(width_us)},D{ch}={int(delay_us)}"
            for ch, delay_us, width_us in pulses
        ) + f",PHASE={int(phase_delay)}\n"

        self.log_message(f"Sending config: {cmd.strip()}", "APP")
        try:
            self.serial_port.write(cmd.encode())
            self.serial_port.flush()
        except Exception as e:
            self.log_message(f"Failed to send config: {e}", "ERROR")

    def start_pulse(self):
        """Send START command to begin pulse sequence."""
        if not self.ensure_serial():
            return
        try:
            self.serial_port.write(b"START\n")
            self.serial_port.flush()
            self.log_message("Sent START command", "APP")
            self.running = True
            self.update_button_states()
        except Exception as e:
            self.log_message(f"Failed to send START: {e}", "ERROR")

    def arm_external_trigger(self):
        """Send ARM command to wait for external trigger."""
        if not self.ensure_serial():
            return
        try:
            self.serial_port.write(b"ARM\n")
            self.serial_port.flush()
            self.log_message("Sent ARM command (waiting for external trigger)", "APP")
            self.running = True
            self.update_button_states()
        except Exception as e:
            self.log_message(f"Failed to send ARM: {e}", "ERROR")

    def stop_sequence(self):
        """Send STOP command to halt pulse sequence."""
        if not self.ensure_serial():
            return
        try:
            self.serial_port.write(b"STOP\n")
            self.serial_port.flush()
            self.log_message("Sent STOP command", "APP")
            self.running = False
            self.update_button_states()
        except Exception as e:
            self.log_message(f"Failed to send STOP: {e}", "ERROR")

    def ensure_serial(self):
        """Ensure serial port is connected, else try connecting."""
        if not (self.serial_port and self.serial_port.is_open):
            return self.connect_serial()
        return True

    def get_channel_settings(self):
        """Retrieve width and delay values for each channel converted to microseconds."""
        settings = []
        for ch in range(1, 5):
            width_val = self.inputs[f"CH{ch}_width"].value()
            width_unit = self.inputs[f"CH{ch}_width_unit"].currentText()
            delay_val = self.inputs[f"CH{ch}_delay"].value()
            delay_unit = self.inputs[f"CH{ch}_delay_unit"].currentText()
            width_us = width_val * UNITS[width_unit]
            delay_us = delay_val * UNITS[delay_unit]
            settings.append((ch, delay_us, width_us))
        return settings

    def get_phase_delay_us(self):
        """Get phase delay value in microseconds."""
        val = self.phase_delay.value()
        unit = self.phase_unit.currentText()
        return val * UNITS[unit]

    def update_button_states(self):
        """Ensure all buttons remain enabled at all times."""
        self.send_btn.setEnabled(True)
        self.start_btn.setEnabled(True)
        self.arm_btn.setEnabled(True)
        self.stop_btn.setEnabled(True)
        self.plot_btn.setEnabled(True)

    def log_message(self, message, source="APP"):
        """Append a timestamped message to the console output."""
        timestamp = datetime.now().strftime("%H:%M:%S.%f")[:-3]
        formatted_msg = f"[{timestamp}] {source}: {message}"
        self.console_output.append(formatted_msg)
        cursor = self.console_output.textCursor()
        cursor.movePosition(QTextCursor.End)
        self.console_output.setTextCursor(cursor)

    def plot_pulses(self):
        """Plot the pulse waveform and calculate/display repetition rate in Hz."""
        self.ax.clear()
        self.ax.set_facecolor("#1e1e1e")
        self.ax.tick_params(colors='white', labelcolor='white')

        pulses = self.get_channel_settings()
        phase_delay_us = self.get_phase_delay_us()

        # Calculate end of last rising edge (max delay + width of pulses)
        last_rising_end_us = max((delay + width) for (_, delay, width) in pulses)

        # Phase delay position is last rising edge + phase delay
        phase_x_us = last_rising_end_us + phase_delay_us

        max_time_us = max(last_rising_end_us, phase_x_us)
        unit, scale = self.get_unit_and_scale(max_time_us)

        for ch, delay_us, width_us in pulses:
            if width_us == 0:
                continue
            start = delay_us / scale
            end = (delay_us + width_us) / scale
            color = self.channel_colors.get(ch, '#FFFFFF')
            # Vertical rising edge
            self.ax.plot([start, start], [0, 1], color=color, linewidth=2)
            # Horizontal high pulse
            self.ax.plot([start, end], [1, 1], color=color, linewidth=4, label=f'Channel {ch}')
            # Vertical falling edge
            self.ax.plot([end, end], [1, 0], color=color, linewidth=2)

        # Plot phase delay line at last rising edge + phase delay
        phase_x = phase_x_us / scale
        self.ax.axvline(x=phase_x, color='red', linestyle='--', linewidth=2, label='Phase Delay')

        self.ax.set_xlabel(f"Time ({unit})", color="white")
        self.ax.set_ylabel("State", color="white")
        self.ax.set_title("Pulse Waveform with Phase Delay", color="white")
        self.ax.set_yticks([0, 1])
        self.ax.set_yticklabels(['0 (Low)', '1 (High)'])
        self.ax.set_ylim(-0.1, 1.5)
        self.ax.grid(True, color="#444")
        handles, labels = self.ax.get_legend_handles_labels()
        if handles:
            self.ax.legend(handles, labels, facecolor='#1e1e1e', labelcolor='white')
        self.canvas.draw()

        # Calculate repetition rate in Hz (pulses per second)
        total_duration_s = max_time_us / 1_000_000
        repetition_rate_hz = 1 / total_duration_s if total_duration_s > 0 else 0
        self.repetition_label.setText(f"Repetition Rate: {repetition_rate_hz:.1f} Hz")

    def get_unit_and_scale(self, max_us):
        """Determine appropriate time unit and scale for plotting."""
        if max_us >= 2_000_000:
            return 's', 1_000_000
        elif max_us >= 2000:
            return 'ms', 1000
        return 'µs', 1

    def closeEvent(self, event):
        """Cleanup on window close."""
        self.stop_sequence()
        if self.serial_port and self.serial_port.is_open:
            self.log_message("Closing serial port...", "APP")
            self.serial_port.close()
        event.accept()

    def apply_dark_theme(self):
        """Apply dark theme colors and styles to the application."""
        dark_palette = QPalette()
        dark_palette.setColor(QPalette.Window, QColor("#121212"))
        dark_palette.setColor(QPalette.WindowText, QColor("#F0F0F0"))
        dark_palette.setColor(QPalette.Base, QColor("#1E1E1E"))
        dark_palette.setColor(QPalette.AlternateBase, QColor("#1E1E1E"))
        dark_palette.setColor(QPalette.ToolTipBase, QColor("#F0F0F0"))
        dark_palette.setColor(QPalette.ToolTipText, QColor("#F0F0F0"))
        dark_palette.setColor(QPalette.Text, QColor("#F0F0F0"))
        dark_palette.setColor(QPalette.Button, QColor("#1E1E1E"))
        dark_palette.setColor(QPalette.ButtonText, QColor("#F0F0F0"))
        dark_palette.setColor(QPalette.BrightText, QColor("#FF0000"))
        dark_palette.setColor(QPalette.Highlight, QColor("#007ACC"))
        dark_palette.setColor(QPalette.HighlightedText, QColor("#FFFFFF"))
        QApplication.instance().setPalette(dark_palette)

        style = """
            QPushButton {
                background-color: #007ACC;
                color: white;
                border-radius: 5px;
                padding: 6px;
            }
            QPushButton:hover {
                background-color: #005F99;
            }
            QPushButton:disabled {
                background-color: #555555;
            }
            QGroupBox {
                font-weight: bold;
                color: #F0F0F0;
                border: 1px solid #444;
                margin-top: 10px;
            }
            QLabel {
                color: #F0F0F0;
            }
            QSpinBox, QComboBox {
                background-color: #1E1E1E;
                color: #F0F0F0;
                border: 1px solid #555;
            }
        """
        self.setStyleSheet(style)

    # Placeholder methods for spinbox change signals to avoid errors
    def on_width_changed(self, val, idx):
        pass
    def on_delay_changed(self, val, idx):
        pass

if __name__ == '__main__':
    app = QApplication(sys.argv)
    gui = PulseGUI()
    gui.show()
    sys.exit(app.exec_())

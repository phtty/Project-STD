import os
import sys
from dataclasses import dataclass

from PySide6.QtCore import QProcess, QProcessEnvironment, QTimer
from PySide6.QtSerialPort import QSerialPort, QSerialPortInfo
from PySide6.QtWidgets import (
    QApplication,
    QComboBox,
    QFormLayout,
    QGridLayout,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMainWindow,
    QMessageBox,
    QPushButton,
    QPlainTextEdit,
    QSpinBox,
    QTabWidget,
    QTextEdit,
    QVBoxLayout,
    QWidget,
)


@dataclass(frozen=True)
class CommandSpec:
    title: str
    cmd: str


CMD_CLEAR = CommandSpec("清屏", "5")
CMD_FULLSCREEN = CommandSpec("全屏显示", "4")
CMD_ONE_LINE = CommandSpec("单行显示", "3")
CMD_BRIGHTNESS = CommandSpec("亮度调节", "8")
CMD_VOLUME = CommandSpec("音量调节", "9")
CMD_PERIPHERAL = CommandSpec("外设控制", "A")
CMD_VOICE = CommandSpec("语音播报", "B")


def to_hex(data: bytes) -> str:
    return " ".join(f"{b:02X}" for b in data)


class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("青海协议上位机测试工具")
        self.resize(1280, 820)

        self.port = QSerialPort(self)
        self.port.readyRead.connect(self.on_ready_read)
        self.port.errorOccurred.connect(self.on_serial_error)

        self.last_frame = b""

        root = QWidget(self)
        self.setCentralWidget(root)
        main_layout = QVBoxLayout(root)

        self.tabs = QTabWidget(self)
        main_layout.addWidget(self.tabs)

        self._build_serial_tab()
        self._build_command_tab()
        self._build_monitor_tab()

        self.refresh_ports()
        self.refresh_preview()

    # ---------------- serial ----------------
    def _build_serial_tab(self):
        tab = QWidget(self)
        layout = QVBoxLayout(tab)

        box = QGroupBox("串口连接", tab)
        grid = QGridLayout(box)

        self.port_combo = QComboBox(box)
        self.port_combo.setEditable(False)
        self.refresh_btn = QPushButton("刷新串口", box)
        self.baud_spin = QSpinBox(box)
        self.baud_spin.setRange(1200, 921600)
        self.baud_spin.setSingleStep(100)
        self.baud_spin.setValue(9600)

        self.stop_bits_combo = QComboBox(box)
        self.stop_bits_combo.addItems(["1", "2"])
        self.parity_combo = QComboBox(box)
        self.parity_combo.addItems(["None", "Even", "Odd"])
        self.data_bits_edit = QLineEdit("8", box)
        self.data_bits_edit.setReadOnly(True)

        self.open_btn = QPushButton("打开串口", box)
        self.close_btn = QPushButton("关闭串口", box)
        self.serial_status = QLabel("未连接", box)
        self.port_hint = QLabel("提示：串口列表来自系统当前已接入的串口设备", box)

        grid.addWidget(QLabel("串口", box), 0, 0)
        grid.addWidget(self.port_combo, 0, 1)
        grid.addWidget(self.refresh_btn, 0, 2)
        grid.addWidget(QLabel("波特率", box), 1, 0)
        grid.addWidget(self.baud_spin, 1, 1)
        grid.addWidget(QLabel("数据位", box), 2, 0)
        grid.addWidget(self.data_bits_edit, 2, 1)
        grid.addWidget(QLabel("停止位", box), 3, 0)
        grid.addWidget(self.stop_bits_combo, 3, 1)
        grid.addWidget(QLabel("校验", box), 4, 0)
        grid.addWidget(self.parity_combo, 4, 1)
        grid.addWidget(self.open_btn, 5, 0)
        grid.addWidget(self.close_btn, 5, 1)
        grid.addWidget(self.serial_status, 6, 0, 1, 3)
        grid.addWidget(self.port_hint, 7, 0, 1, 3)

        layout.addWidget(box)
        layout.addStretch(1)

        self.refresh_btn.clicked.connect(self.refresh_ports)
        self.open_btn.clicked.connect(self.open_port)
        self.close_btn.clicked.connect(self.close_port)
        self.tabs.addTab(tab, "串口")

    def refresh_ports(self):
        current = self.port_combo.currentData() or ""
        self.port_combo.clear()
        ports = QSerialPortInfo.availablePorts()
        for p in ports:
            port_name = p.portName()
            sys_loc = p.systemLocation()
            if not port_name:
                continue
            # 仅显示外接/实际接入的串口，屏蔽系统自带的 ttyS* 等内部串口。
            if not (sys_loc.startswith("/dev/ttyUSB") or sys_loc.startswith("/dev/ttyACM") or sys_loc.startswith("/dev/ttyAMA") or sys_loc.startswith("/dev/serial/by-id")):
                continue
            label = f"{port_name}"
            if p.description():
                label = f"{port_name} - {p.description()}"
            self.port_combo.addItem(label, port_name)
        if current:
            idx = self.port_combo.findData(current)
            if idx >= 0:
                self.port_combo.setCurrentIndex(idx)
        if self.port_combo.count() == 0:
            self.port_combo.addItem("无可用串口", "")
            self.port_combo.setEnabled(False)
            self.open_btn.setEnabled(False)
        else:
            self.port_combo.setEnabled(True)
            self.open_btn.setEnabled(True)

    def _current_port_name(self) -> str:
        return self.port_combo.currentData() or ""

    def open_port(self):
        if self.port.isOpen():
            self.port.close()

        name = self._current_port_name()
        if not name:
            QMessageBox.warning(self, "提示", "未发现可用串口")
            return

        self.port.setPortName(name)
        self.port.setBaudRate(self.baud_spin.value())
        self.port.setDataBits(QSerialPort.DataBits.Data8)
        self.port.setStopBits(QSerialPort.StopBits.OneStop if self.stop_bits_combo.currentText() == "1" else QSerialPort.StopBits.TwoStop)
        parity_map = {
            "None": QSerialPort.Parity.NoParity,
            "Even": QSerialPort.Parity.EvenParity,
            "Odd": QSerialPort.Parity.OddParity,
        }
        self.port.setParity(parity_map[self.parity_combo.currentText()])
        self.port.setFlowControl(QSerialPort.FlowControl.NoFlowControl)

        if not self.port.open(QSerialPort.OpenModeFlag.ReadWrite):
            QMessageBox.critical(self, "错误", f"打开串口失败：{self.port.errorString()}")
            return

        self.serial_status.setText(f"已连接：{name} @ {self.baud_spin.value()}")
        self.log_tx(f"[OPEN] {name} @ {self.baud_spin.value()}")

    def close_port(self):
        if self.port.isOpen():
            self.port.close()
            self.serial_status.setText("未连接")
            self.log_tx("[CLOSE]")

    def on_ready_read(self):
        data = bytes(self.port.readAll())
        if not data:
            return
        self.rx_monitor.append(f"< {to_hex(data)}")

    def on_serial_error(self, error):
        if error == QSerialPort.SerialPortError.NoError:
            return
        if self.port.isOpen() and error in (
            QSerialPort.SerialPortError.ResourceError,
            QSerialPort.SerialPortError.PermissionError,
        ):
            self.serial_status.setText("串口异常，已断开")
            self.port.close()

    # ---------------- command ----------------
    def _build_command_tab(self):
        tab = QWidget(self)
        layout = QVBoxLayout(tab)

        self.command_tabs = QTabWidget(tab)
        layout.addWidget(self.command_tabs)

        self.clear_tab = self._build_clear_tab()
        self.full_tab = self._build_full_tab()
        self.one_line_tab = self._build_one_line_tab()
        self.brightness_tab = self._build_brightness_tab()
        self.volume_tab = self._build_volume_tab()
        self.peripheral_tab = self._build_peripheral_tab()
        self.voice_tab = self._build_voice_tab()

        self.command_tabs.addTab(self.clear_tab, CMD_CLEAR.title)
        self.command_tabs.addTab(self.full_tab, CMD_FULLSCREEN.title)
        self.command_tabs.addTab(self.one_line_tab, CMD_ONE_LINE.title)
        self.command_tabs.addTab(self.brightness_tab, CMD_BRIGHTNESS.title)
        self.command_tabs.addTab(self.volume_tab, CMD_VOLUME.title)
        self.command_tabs.addTab(self.peripheral_tab, CMD_PERIPHERAL.title)
        self.command_tabs.addTab(self.voice_tab, CMD_VOICE.title)

        self.tabs.addTab(tab, "协议帧生成")
        self.command_tabs.currentChanged.connect(self.refresh_preview)

    def _make_preview_block(self):
        row = QWidget(self)
        hl = QHBoxLayout(row)
        hl.setContentsMargins(0, 0, 0, 0)
        edit = QLineEdit(row)
        edit.setReadOnly(True)
        edit.setPlaceholderText("这里显示拼接后的协议帧")
        status = QLabel("0 bytes", row)
        hl.addWidget(edit, 1)
        hl.addWidget(status)
        return row, edit, status

    def _add_common_buttons(self, layout, send_slot, copy_slot):
        btn_row = QWidget(self)
        hl = QHBoxLayout(btn_row)
        hl.setContentsMargins(0, 0, 0, 0)
        send_btn = QPushButton("发送", btn_row)
        copy_btn = QPushButton("复制帧", btn_row)
        refresh_btn = QPushButton("刷新预览", btn_row)
        send_btn.clicked.connect(send_slot)
        copy_btn.clicked.connect(copy_slot)
        refresh_btn.clicked.connect(self.refresh_preview)
        hl.addWidget(send_btn)
        hl.addWidget(copy_btn)
        hl.addWidget(refresh_btn)
        hl.addStretch(1)
        layout.addWidget(btn_row)

    def _build_clear_tab(self):
        tab = QWidget(self)
        layout = QVBoxLayout(tab)
        layout.addWidget(QLabel("清屏指令无需输入文本，软件自动拼接完整帧。", tab))
        self.clear_preview_box, self.clear_preview_edit, self.clear_preview_status = self._make_preview_block()
        layout.addWidget(self.clear_preview_box)
        self._add_common_buttons(layout, self.send_clear, self.copy_current_preview)
        layout.addStretch(1)
        return tab

    def _build_full_tab(self):
        tab = QWidget(self)
        form_box = QGroupBox("全屏显示参数", tab)
        form = QFormLayout(form_box)
        self.full_text = QTextEdit(form_box)
        self.full_text.setPlaceholderText("请输入需要显示的文本，软件自动拼接命令字 4")
        self.full_color = QComboBox(form_box)
        self.full_color.addItems(["0-红色", "1-绿色", "2-黄色"])
        self.full_x = QSpinBox(form_box)
        self.full_x.setRange(0, 255)
        self.full_x.setValue(0)
        self.full_y = QSpinBox(form_box)
        self.full_y.setRange(0, 255)
        self.full_y.setValue(0)
        form.addRow("文本", self.full_text)
        form.addRow("颜色", self.full_color)
        form.addRow("X", self.full_x)
        form.addRow("Y", self.full_y)
        self.full_preview_box, self.full_preview_edit, self.full_preview_status = self._make_preview_block()

        layout = QVBoxLayout(tab)
        layout.addWidget(form_box)
        layout.addWidget(self.full_preview_box)
        self._add_common_buttons(layout, self.send_full, self.copy_current_preview)
        layout.addStretch(1)

        self.full_text.textChanged.connect(self.refresh_preview)
        self.full_color.currentIndexChanged.connect(self.refresh_preview)
        self.full_x.valueChanged.connect(self.refresh_preview)
        self.full_y.valueChanged.connect(self.refresh_preview)
        return tab

    def _build_one_line_tab(self):
        tab = QWidget(self)
        form_box = QGroupBox("单行显示参数", tab)
        form = QFormLayout(form_box)
        self.one_text = QLineEdit(form_box)
        self.one_text.setPlaceholderText("请输入单行文本")
        self.one_row = QSpinBox(form_box)
        self.one_row.setRange(1, 4)
        self.one_row.setValue(1)
        self.one_color = QComboBox(form_box)
        self.one_color.addItems(["0-红色", "1-绿色", "2-黄色"])
        form.addRow("文本", self.one_text)
        form.addRow("行号", self.one_row)
        form.addRow("颜色", self.one_color)
        self.one_preview_box, self.one_preview_edit, self.one_preview_status = self._make_preview_block()

        layout = QVBoxLayout(tab)
        layout.addWidget(form_box)
        layout.addWidget(self.one_preview_box)
        self._add_common_buttons(layout, self.send_one_line, self.copy_current_preview)
        layout.addStretch(1)

        self.one_text.textChanged.connect(self.refresh_preview)
        self.one_row.valueChanged.connect(self.refresh_preview)
        self.one_color.currentIndexChanged.connect(self.refresh_preview)
        return tab

    def _build_brightness_tab(self):
        tab = QWidget(self)
        form_box = QGroupBox("亮度调节", tab)
        form = QFormLayout(form_box)
        self.brightness_level = QSpinBox(form_box)
        self.brightness_level.setRange(1, 5)
        self.brightness_level.setValue(3)
        form.addRow("亮度等级", self.brightness_level)
        self.brightness_preview_box, self.brightness_preview_edit, self.brightness_preview_status = self._make_preview_block()
        layout = QVBoxLayout(tab)
        layout.addWidget(form_box)
        layout.addWidget(self.brightness_preview_box)
        self._add_common_buttons(layout, self.send_brightness, self.copy_current_preview)
        layout.addStretch(1)
        self.brightness_level.valueChanged.connect(self.refresh_preview)
        return tab

    def _build_volume_tab(self):
        tab = QWidget(self)
        form_box = QGroupBox("音量调节", tab)
        form = QFormLayout(form_box)
        self.volume_level = QSpinBox(form_box)
        self.volume_level.setRange(1, 5)
        self.volume_level.setValue(3)
        form.addRow("音量等级", self.volume_level)
        self.volume_preview_box, self.volume_preview_edit, self.volume_preview_status = self._make_preview_block()
        layout = QVBoxLayout(tab)
        layout.addWidget(form_box)
        layout.addWidget(self.volume_preview_box)
        self._add_common_buttons(layout, self.send_volume, self.copy_current_preview)
        layout.addStretch(1)
        self.volume_level.valueChanged.connect(self.refresh_preview)
        return tab

    def _build_peripheral_tab(self):
        tab = QWidget(self)
        form_box = QGroupBox("外设控制", tab)
        form = QFormLayout(form_box)
        self.peri_green = QComboBox(form_box)
        self.peri_green.addItems(["关", "开"])
        self.peri_red = QComboBox(form_box)
        self.peri_red.addItems(["关", "开"])
        self.peri_yellow = QComboBox(form_box)
        self.peri_yellow.addItems(["关", "开"])
        form.addRow("绿灯", self.peri_green)
        form.addRow("红灯", self.peri_red)
        form.addRow("黄闪", self.peri_yellow)
        self.peri_preview_box, self.peri_preview_edit, self.peri_preview_status = self._make_preview_block()
        layout = QVBoxLayout(tab)
        layout.addWidget(form_box)
        layout.addWidget(self.peri_preview_box)
        self._add_common_buttons(layout, self.send_peripheral, self.copy_current_preview)
        layout.addStretch(1)
        self.peri_green.currentIndexChanged.connect(self.refresh_preview)
        self.peri_red.currentIndexChanged.connect(self.refresh_preview)
        self.peri_yellow.currentIndexChanged.connect(self.refresh_preview)
        return tab

    def _build_voice_tab(self):
        tab = QWidget(self)
        form_box = QGroupBox("语音播报", tab)
        form = QFormLayout(form_box)
        self.voice_type = QComboBox(form_box)
        self.voice_type.addItems(["文明用语", "费额播报"])
        self.voice_text = QLineEdit(form_box)
        self.voice_text.setPlaceholderText("文明用语：输入索引；费额播报：输入金额")
        self.voice_index = QSpinBox(form_box)
        self.voice_index.setRange(0, 9)
        self.voice_index.setValue(0)
        form.addRow("类型", self.voice_type)
        form.addRow("文本/金额", self.voice_text)
        form.addRow("索引", self.voice_index)
        self.voice_preview_box, self.voice_preview_edit, self.voice_preview_status = self._make_preview_block()
        layout = QVBoxLayout(tab)
        layout.addWidget(form_box)
        layout.addWidget(self.voice_preview_box)
        self._add_common_buttons(layout, self.send_voice, self.copy_current_preview)
        layout.addStretch(1)
        self.voice_type.currentIndexChanged.connect(self.on_voice_mode_changed)
        self.voice_text.textChanged.connect(self.refresh_preview)
        self.voice_index.valueChanged.connect(self.refresh_preview)
        self.on_voice_mode_changed()
        return tab

    def on_voice_mode_changed(self):
        if self.voice_type.currentText() == "文明用语":
            self.voice_index.setEnabled(True)
            self.voice_text.setPlaceholderText("请输入文明用语索引")
        else:
            self.voice_index.setEnabled(False)
            self.voice_text.setPlaceholderText("请输入金额，如 12345")
        self.refresh_preview()

    # ---------------- frame builder ----------------
    @staticmethod
    def _wrap_frame(cmd: str, data: bytes) -> bytes:
        return bytes([0x7B]) + cmd.encode("ascii") + bytes([len(data) & 0xFF]) + data + bytes([0x7D])

    def build_clear_frame(self) -> bytes:
        return self._wrap_frame(CMD_CLEAR.cmd, b"")

    def build_full_frame(self) -> bytes:
        text = self.full_text.toPlainText().replace("\r\n", "\n").replace("\r", "\n")
        data = bytes([ord(str(self.full_color.currentIndex()))]) + bytes([self.full_x.value() & 0xFF]) + bytes([self.full_y.value() & 0xFF]) + text.encode("ascii", errors="ignore")
        return self._wrap_frame(CMD_FULLSCREEN.cmd, data)

    def build_one_frame(self) -> bytes:
        text = self.one_text.text().strip()
        if not text:
            raise ValueError("单行显示必须输入文本")
        row_ascii = str(self.one_row.value()).encode("ascii")
        color_ascii = str(self.one_color.currentIndex()).encode("ascii")
        data = color_ascii + row_ascii + text.encode("ascii", errors="ignore")
        return self._wrap_frame(CMD_ONE_LINE.cmd, data)

    def build_brightness_frame(self) -> bytes:
        data = str(self.brightness_level.value()).encode("ascii")
        return self._wrap_frame(CMD_BRIGHTNESS.cmd, data)

    def build_volume_frame(self) -> bytes:
        data = str(self.volume_level.value()).encode("ascii")
        return self._wrap_frame(CMD_VOLUME.cmd, data)

    def build_peripheral_frame(self) -> bytes:
        green = 1 if self.peri_green.currentText() == "开" else 0
        red = 2 if self.peri_red.currentText() == "开" else 0
        yellow = 4 if self.peri_yellow.currentText() == "开" else 0
        data = bytes([green | red | yellow])
        return self._wrap_frame(CMD_PERIPHERAL.cmd, data)

    def build_voice_frame(self) -> bytes:
        if self.voice_type.currentText() == "文明用语":
            data = str(self.voice_index.value()).encode("ascii")
            return self._wrap_frame(CMD_VOICE.cmd, data)
        amount = self.voice_text.text().strip()
        if not amount:
            raise ValueError("费额播报必须输入金额")
        clean = "".join(ch for ch in amount if ch.isdigit())
        if not clean:
            raise ValueError("金额必须为数字")
        data = b"0" + clean.encode("ascii")[:5]
        return self._wrap_frame(CMD_VOICE.cmd, data)

    def _preview_target(self):
        current = self.command_tabs.currentWidget()
        if current is self.clear_tab:
            return self.clear_preview_edit, self.clear_preview_status
        if current is self.full_tab:
            return self.full_preview_edit, self.full_preview_status
        if current is self.one_line_tab:
            return self.one_preview_edit, self.one_preview_status
        if current is self.brightness_tab:
            return self.brightness_preview_edit, self.brightness_preview_status
        if current is self.volume_tab:
            return self.volume_preview_edit, self.volume_preview_status
        if current is self.peripheral_tab:
            return self.peri_preview_edit, self.peri_preview_status
        return self.voice_preview_edit, self.voice_preview_status

    def _build_current_frame(self) -> bytes:
        current = self.command_tabs.currentWidget()
        if current is self.clear_tab:
            return self.build_clear_frame()
        if current is self.full_tab:
            return self.build_full_frame()
        if current is self.one_line_tab:
            return self.build_one_frame()
        if current is self.brightness_tab:
            return self.build_brightness_frame()
        if current is self.volume_tab:
            return self.build_volume_frame()
        if current is self.peripheral_tab:
            return self.build_peripheral_frame()
        return self.build_voice_frame()

    def refresh_preview(self):
        edit, status = self._preview_target()
        try:
            frame = self._build_current_frame()
            edit.setText(to_hex(frame))
            status.setText(f"{len(frame)} bytes")
            self.last_frame = frame
        except Exception as exc:
            edit.setText(f"ERR: {exc}")
            status.setText("--")

    def send_clear(self):
        self._send_frame(self.build_clear_frame())

    def send_full(self):
        self._send_frame(self.build_full_frame())

    def send_one_line(self):
        self._send_frame(self.build_one_frame())

    def send_brightness(self):
        self._send_frame(self.build_brightness_frame())

    def send_volume(self):
        self._send_frame(self.build_volume_frame())

    def send_peripheral(self):
        self._send_frame(self.build_peripheral_frame())

    def send_voice(self):
        self._send_frame(self.build_voice_frame())

    def _send_frame(self, frame: bytes):
        if not self.port.isOpen():
            QMessageBox.warning(self, "提示", "请先打开串口")
            return
        self.port.write(frame)
        self.log_tx(f"> {to_hex(frame)}")

    def copy_current_preview(self):
        try:
            QApplication.clipboard().setText(to_hex(self._build_current_frame()))
        except Exception as exc:
            QMessageBox.warning(self, "提示", str(exc))

    # ---------------- monitor ----------------
    def _build_monitor_tab(self):
        tab = QWidget(self)
        layout = QVBoxLayout(tab)
        self.rx_monitor = QTextEdit(tab)
        self.rx_monitor.setReadOnly(True)
        self.rx_monitor.setPlaceholderText("接收数据")
        self.tx_monitor = QTextEdit(tab)
        self.tx_monitor.setReadOnly(True)
        self.tx_monitor.setPlaceholderText("发送记录")

        btn_row = QWidget(tab)
        hl = QHBoxLayout(btn_row)
        hl.setContentsMargins(0, 0, 0, 0)
        clear_btn = QPushButton("清空监视", btn_row)
        clear_btn.clicked.connect(self.clear_monitor)
        hl.addWidget(clear_btn)
        hl.addStretch(1)

        layout.addWidget(QLabel("接收监视", tab))
        layout.addWidget(self.rx_monitor, 2)
        layout.addWidget(QLabel("发送记录", tab))
        layout.addWidget(self.tx_monitor, 1)
        layout.addWidget(btn_row)
        self.tabs.addTab(tab, "监视")

    def clear_monitor(self):
        self.rx_monitor.clear()
        self.tx_monitor.clear()

    def log_tx(self, text: str):
        self.tx_monitor.append(text)


def main():
    app = QApplication(sys.argv)
    app.setStyle("Fusion")
    win = MainWindow()
    win.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()

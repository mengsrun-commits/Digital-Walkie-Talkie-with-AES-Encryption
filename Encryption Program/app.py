from PyQt6.QtWidgets import QWidget, QStackedWidget, QVBoxLayout
from PyQt6.QtCore import QTimer
from pages import LoginApp, ProgramPage, StartPage

class MyApp(QWidget):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("(TEED Gen 10) Walkie-Talkie Encryption Program")
        self._build_ui()

    def _build_ui(self):
        layout = QVBoxLayout()
        layout.setContentsMargins(0, 0, 0, 0)

        self.stack = QStackedWidget()
        self.start_page = StartPage()
        self.login_page = LoginApp()
        self.program_page = ProgramPage()

        self.stack.addWidget(self.start_page)
        self.stack.addWidget(self.login_page)
        self.stack.addWidget(self.program_page)

        self.start_page.on_program_start.connect(self.show_login_page)
        self.start_page.on_preview.connect(lambda: self.show_program_page(from_preview=True))
        self.login_page.login_success.connect(self.show_program_page)
        self.login_page.back_to_start.connect(self.on_back_to_start)
        self.login_page.device_disconnected.connect(self._on_device_disconnected)
        self.program_page.back_to_login.connect(self.show_login_page)
        self.program_page.back_to_start.connect(self.show_start_page)

        layout.addWidget(self.stack)
        self.setLayout(layout)
        self.show_start_page()

    def show_start_page(self):
        self.stack.setCurrentWidget(self.start_page)

    def on_back_to_start(self):
        self.show_start_page()
        
    def show_login_page(self):
        self.login_page.start_connection()
        self.stack.setCurrentWidget(self.login_page)

    def show_program_page(self, from_preview=False):
        self.program_page.set_back_mode(from_preview)
        self.program_page.reload_channel_config()
        if from_preview:
            self.program_page.set_device_name("Walkie-Talkie Device")
            self.program_page.set_upload_port("")
        else:
            selected_port = self.login_page._selected_port()
            self.program_page.set_device_name(self.login_page._format_device_label(selected_port))
            self.program_page.set_upload_port(selected_port)
            self.login_page.stop_connection()

        # Ensure ProgramPage UI is reset when entering
        self.program_page.header_widget.show()
        self.program_page.content_widget.show()
        self.program_page.back_button.show()
        self.program_page.set_configuration_button.show()
        self.program_page.disconnect_label.hide()
        self.program_page._apply_preview_mode()
        
        self.stack.setCurrentWidget(self.program_page)
        QTimer.singleShot(0, self.login_page.clear_credentials)

    def _on_device_disconnected(self, port):
        """Handler for when a device is physically unplugged."""
        if self.stack.currentWidget() == self.program_page:
            # If we are on program page, show message and exit to start
            self.program_page.show_disconnect_and_exit()
        

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
        self.program_page.back_to_login.connect(self.show_login_page)
        self.program_page.back_to_start.connect(self.show_start_page)

        layout.addWidget(self.stack)
        self.setLayout(layout)
        self.show_start_page()

    def show_start_page(self):
        self.stack.setCurrentWidget(self.start_page)

    def on_back_to_start(self):
        self.login_page.stop_connection()
        self.show_start_page()
        
    def show_login_page(self):
        self.login_page.start_connection()
        self.stack.setCurrentWidget(self.login_page)

    def show_program_page(self, from_preview=False):
        self.program_page.set_back_mode(from_preview)
        self.stack.setCurrentWidget(self.program_page)
        QTimer.singleShot(0, self.login_page.clear_credentials)
        
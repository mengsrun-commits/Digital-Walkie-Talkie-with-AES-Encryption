from PyQt6.QtWidgets import (
    QWidget, QVBoxLayout, QHBoxLayout,
    QLabel, QPushButton, QSizePolicy
)
from PyQt6.QtCore import Qt, pyqtSignal
from PyQt6.QtGui import QFont

class StartPage(QWidget):
    on_program_start = pyqtSignal()
    on_preview = pyqtSignal()
    
    BUTTON_WIDTH = 120
    
    def __init__(self):
        super().__init__()
        self.setWindowTitle("Start Page")
        self._build_ui()

    def _build_ui(self):
        main_layout = QVBoxLayout()
        main_layout.setContentsMargins(24, 24, 24, 24)
        
        self.welcome_widget = QWidget()
        self.welcome_layout = QVBoxLayout()
        self.welcome_layout.setSpacing(14)
        self.welcome_layout.setContentsMargins(0, 0, 0, 0)
        self.welcome_widget.setLayout(self.welcome_layout)
        self.welcome_widget.setMinimumWidth(560)
        self.welcome_widget.setMaximumWidth(900)
        self.welcome_widget.setSizePolicy(QSizePolicy.Policy.Fixed, QSizePolicy.Policy.Fixed)
        
        self.title = QLabel("Walkie-Talkie Encryption Program")
        self.title.setFont(QFont("Arial", 18, QFont.Weight.Bold))
        self.title.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.welcome_layout.addWidget(self.title, alignment=Qt.AlignmentFlag.AlignHCenter)

        self.buttons_layout = QHBoxLayout()
        self.start_btn = QPushButton("Start Program")
        self.start_btn.setFixedHeight(36)
        self.start_btn.setFixedWidth(self.BUTTON_WIDTH)
        self.start_btn.clicked.connect(self.on_start_clicked)
        
        self.preview_btn = QPushButton("Preview")
        self.preview_btn.setFixedHeight(36)
        self.preview_btn.setFixedWidth(self.BUTTON_WIDTH)
        self.preview_btn.clicked.connect(self.on_preview_clicked)
        
        self.buttons_layout.addWidget(self.start_btn)
        self.buttons_layout.addWidget(self.preview_btn)
        self.welcome_layout.addLayout(self.buttons_layout)
        self.welcome_layout.setAlignment(self.buttons_layout, Qt.AlignmentFlag.AlignHCenter)
        
        main_layout.addStretch()
        main_layout.addWidget(self.welcome_widget, alignment=Qt.AlignmentFlag.AlignHCenter)
        main_layout.addStretch()
        self.setLayout(main_layout)
    
    def on_start_clicked(self):
        self.on_program_start.emit()
        
    def on_preview_clicked(self):
        self.on_preview.emit()
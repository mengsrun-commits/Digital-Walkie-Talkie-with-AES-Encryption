import sys
from PyQt6.QtWidgets import (
    QApplication, QWidget, QVBoxLayout, QHBoxLayout,
    QLabel, QLineEdit, QPushButton, QTextEdit, QMessageBox,
    QCheckBox, QComboBox, QSpinBox, QGroupBox
)
from PyQt6.QtCore import Qt
from PyQt6.QtGui import QFont


class MyApp(QWidget):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("PyQt6 - Buttons & Inputs Demo")
        self.setMinimumWidth(450)
        self.setup_ui()

    def setup_ui(self):
        main_layout = QVBoxLayout()
        main_layout.setSpacing(30)
        main_layout.setContentsMargins(20, 20, 20, 20)

        # --- Title ---
        title = QLabel("PyQt6 Input Demo")
        title.setFont(QFont("Arial", 16, QFont.Weight.Bold))
        title.setAlignment(Qt.AlignmentFlag.AlignCenter)
        main_layout.addWidget(title)

        # ── Group 1: Text Inputs ──────────────────────────────
        group1 = QGroupBox("Text Inputs")
        g1_layout = QVBoxLayout()

        # Name input
        g1_layout.addWidget(QLabel("Full Name:"))
        self.name_input = QLineEdit()
        self.name_input.setPlaceholderText("Enter your name...")
        g1_layout.addWidget(self.name_input)

        # Password input
        g1_layout.addWidget(QLabel("Password:"))
        self.password_input = QLineEdit()
        self.password_input.setPlaceholderText("Enter password...")
        self.password_input.setEchoMode(QLineEdit.EchoMode.Password)
        g1_layout.addWidget(self.password_input)

        # Multi-line input
        g1_layout.addWidget(QLabel("Message:"))
        self.message_input = QTextEdit()
        self.message_input.setPlaceholderText("Type a message here...")
        self.message_input.setFixedHeight(80)
        g1_layout.addWidget(self.message_input)

        group1.setLayout(g1_layout)
        main_layout.addWidget(group1)

        # ── Group 2: Other Inputs ─────────────────────────────
        group2 = QGroupBox("Other Inputs")
        g2_layout = QVBoxLayout()

        # Dropdown (ComboBox)
        g2_layout.addWidget(QLabel("Select a language:"))
        self.combo = QComboBox()
        self.combo.addItems(["Python", "C++", "JavaScript", "Rust", "Go"])
        g2_layout.addWidget(self.combo)

        # SpinBox (number input)
        g2_layout.addWidget(QLabel("Pick a number (1–100):"))
        self.spinbox = QSpinBox()
        self.spinbox.setRange(1, 100)
        self.spinbox.setValue(42)
        g2_layout.addWidget(self.spinbox)

        # Checkbox
        self.checkbox = QCheckBox("I agree to the terms and conditions")
        g2_layout.addWidget(self.checkbox)

        group2.setLayout(g2_layout)
        main_layout.addWidget(group2)

        # ── Buttons ───────────────────────────────────────────
        btn_layout = QHBoxLayout()

        self.submit_btn = QPushButton("Submit")
        self.submit_btn.setFixedHeight(36)
        self.submit_btn.clicked.connect(self.on_submit)

        self.clear_btn = QPushButton("Clear")
        self.clear_btn.setFixedHeight(36)
        self.clear_btn.clicked.connect(self.on_clear)

        btn_layout.addWidget(self.submit_btn)
        btn_layout.addWidget(self.clear_btn)
        main_layout.addLayout(btn_layout)

        # ── Output Label ──────────────────────────────────────
        self.output_label = QLabel("")
        self.output_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.output_label.setWordWrap(True)
        self.output_label.setStyleSheet("color: green; font-weight: bold;")
        main_layout.addWidget(self.output_label)

        self.setLayout(main_layout)

    def on_submit(self):
        name = self.name_input.text().strip()
        password = self.password_input.text().strip()
        message = self.message_input.toPlainText().strip()
        language = self.combo.currentText()
        number = self.spinbox.value()
        agreed = self.checkbox.isChecked()

        if not name:
            QMessageBox.warning(self, "Missing Input", "Please enter your name.")
            return

        if not password:
            QMessageBox.warning(self, "Missing Input", "Please enter a password.")
            return

        if not agreed:
            QMessageBox.warning(self, "Terms", "You must agree to the terms.")
            return

        summary = (
            f"Name: {name} | Language: {language} | "
            f"Number: {number} | Message: {message or '(none)'}"
        )
        self.output_label.setText(f"✅ Submitted! {summary}")

    def on_clear(self):
        self.name_input.clear()
        self.password_input.clear()
        self.message_input.clear()
        self.combo.setCurrentIndex(0)
        self.spinbox.setValue(42)
        self.checkbox.setChecked(False)
        self.output_label.setText("")


if __name__ == "__main__":
    app = QApplication(sys.argv)
    app.setStyle("Fusion")  # Clean cross-platform style
    window = MyApp()
    window.show()
    sys.exit(app.exec())
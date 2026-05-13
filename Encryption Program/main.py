from PyQt6.QtWidgets import QApplication
from app import MyApp
import sys

if __name__ == "__main__":
    app = QApplication(sys.argv)
    app.setStyle("Fusion")  # Clean cross-platform style
    window = MyApp()
    window.showMaximized()
    sys.exit(app.exec())
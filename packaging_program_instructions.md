1. Install pyinstaller
2. From the project root:
pyinstaller --onefile --windowed "Encryption Program/main.py"
3. The executable will be located in the "dist" folder.
4. If you want to change the icon, add the --icon flag followed by the path to your .ico file:
pyinstaller --onefile --windowed --icon "Encryption Program/app.ico" "Encryption Program/main.py"
5. --name "...." to add name to the executable
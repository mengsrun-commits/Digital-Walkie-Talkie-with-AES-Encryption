# Secure Digital Walkie-Talkie System

## Overview
This project is a complete, end-to-end solution for secure, encrypted two-way radio communication, seamlessly integrating embedded hardware with a modern desktop application. It brings tactical-level security to radio transmissions by combining advanced cryptography, custom microcontroller firmware, and a user-friendly Python GUI.

## Features
- **Desktop Command Center**
  - Built with **PyQt6** for a multi-page, intuitive GUI (Login, Start, Program screens)
  - AES encryption and PBKDF2HMAC key derivation using the `cryptography` library
  - Serial/USB communication with hardware via `pyserial`
  - Secure provisioning and configuration of radio devices

- **Tactical Hardware**
  - Custom firmware for **ESP32** microcontrollers (C++), handling:
    - Real-time audio capture and processing
    - LCD/OLED screen management
    - Digital radio transmission and reception
  - Support for multiple RF transceivers:
    - **nRF modules** for encrypted digital walkie-talkie functionality
    - **CC1101 sub-1GHz transceivers** for extended range and experimentation

## How It Works
1. The desktop app configures and provisions security keys over a serial connection.
2. The microcontroller captures voice, encrypts it in real time, and transmits it via radio.
3. The receiver picks up the encrypted payload, decrypts it, and plays the audio.

## Technologies Used
- Python (PyQt6, pyserial, cryptography, pyinstaller)
- C++ (ESP32, nRF, CC1101)
- Custom hardware and radio modules

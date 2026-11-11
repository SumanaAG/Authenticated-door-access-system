# Authenticated-door-access-system

Reads an RFID card, checks it against authorized IDs, shows the result on an I2C LCD, and sends an SMS alert through a GSM module when access is denied.

Connections
RFID reader (EM18): UART0, 9600 baud (P0.2, P0.3)
GSM module (SIM900): UART2, 9600 baud (P0.10, P0.11)
LCD : I2C0 (P0.27 SDA, P0.28 SCL)
Door lock / LED: P2.0

Setup
In rfidgsm.c, set authorized[] (card IDs), ALERT_NUMBER and LCD_ADDR (0x4E or 0x7E). Build in Keil uVision

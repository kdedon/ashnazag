; One command to one target, no disconnection.  Everything goes through
; the table at DSA; the script is position independent.

ARCH 720

ABSOLUTE ds_Device = 0
ABSOLUTE ds_MsgOut = 4
ABSOLUTE ds_Cmd = 12
ABSOLUTE ds_Data = 20
ABSOLUTE ds_Status = 28
ABSOLUTE ds_MsgIn = 36

ABSOLUTE ok = 0xff00
ABSOLUTE badphase = 0xff01
ABSOLUTE lost = 0xff02

ENTRY start
ENTRY afterdata

PROC csppc_script:

start:
	SELECT ATN FROM ds_Device, REL(lostsel)
	JUMP REL(bad), WHEN NOT MSG_OUT
	MOVE FROM ds_MsgOut, WHEN MSG_OUT
	JUMP REL(bad), WHEN NOT CMD
	CLEAR ATN
	MOVE FROM ds_Cmd, WHEN CMD
	JUMP REL(dataout), WHEN DATA_OUT
	JUMP REL(datain), WHEN DATA_IN
	JUMP REL(afterdata)
dataout:
	MOVE FROM ds_Data, WHEN DATA_OUT
	JUMP REL(afterdata)
datain:
	MOVE FROM ds_Data, WHEN DATA_IN
; a short transfer stops above with a phase mismatch; the host resumes here
afterdata:
	JUMP REL(status), WHEN STATUS
	JUMP REL(msgin), WHEN MSG_IN
	JUMP REL(bad)
status:
	MOVE FROM ds_Status, WHEN STATUS
	JUMP REL(bad), WHEN NOT MSG_IN
msgin:
	MOVE FROM ds_MsgIn, WHEN MSG_IN
	JUMP REL(done), IF 0x00
	CLEAR ACK
	JUMP REL(afterdata)
done:
	MOVE SCNTL2 & 0x7f TO SCNTL2
	CLEAR ACK
	WAIT DISCONNECT
	INT ok
bad:
	INT badphase
lostsel:
	INT lost

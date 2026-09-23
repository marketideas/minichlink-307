// The "bootloader" blob is (C) WCH.
// Tricky: You need to use wch link to use WCH-LinkRV.
//  you can always uninstall it in device manager.  It will be under USB devices or something like that at the bottom.

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libusb.h"
#include "minichlink.h"

struct LinkEProgrammerStruct
{
	void *internal;
	libusb_device_handle *devh;
	int lasthaltmode; // For non-003 chips
};

static void printChipInfo(enum RiscVChip chip)
{
	switch (chip)
	{
	case CHIP_CH32V10x:
		fprintf(stderr, "Detected: CH32V10x\n");
		break;
	case CHIP_CH57x:
		fprintf(stderr, "Detected: CH57x\n");
		break;
	case CHIP_CH56x:
		fprintf(stderr, "Detected: CH56x\n");
		break;
	case CHIP_CH32V20x:
		fprintf(stderr, "Detected: CH32V20x\n");
		break;
	case CHIP_CH32V30x:
		fprintf(stderr, "Detected: CH32V30x\n");
		break;
	case CHIP_CH58x:
		fprintf(stderr, "Detected: CH58x\n");
		break;
	case CHIP_CH32V003:
		fprintf(stderr, "Detected: CH32V003\n");
		break;
	}
}

static int checkChip(enum RiscVChip chip)
{
	switch (chip)
	{
	case CHIP_CH32V003:
		return 0; // Use direct mode
	case CHIP_CH32V10x:
	case CHIP_CH32V20x:
	case CHIP_CH32V30x:
		return 1; // Use binary blob mode
	case CHIP_CH56x:
	case CHIP_CH57x:
	case CHIP_CH58x:
	default:
		return -1; // Not supported yet
	}
}

// For non-ch32v003 chips.
// static int LEReadBinaryBlob( void * d, uint32_t offset, uint32_t amount, uint8_t * readbuff );
static int InternalLinkEHaltMode(void *d, int mode);
static int LEWriteBinaryBlob(void *d, uint32_t address_to_write, uint32_t len, uint8_t *blob);

#define WCHTIMEOUT 5000
#define WCHCHECK(x)                                                                       \
    if ((status = x)) {                                                                   \
        fprintf(stderr, "Bad USB Operation on " __FILE__ ":%d (%d)\n", __LINE__, status); \
        exit(status);                                                                     \
    }

void wch_link_command(
    libusb_device_handle *devh, const void *command_v, int commandlen, int *transferred, uint8_t *reply, int replymax
)
{
	uint8_t *command = (uint8_t *)command_v;
	uint8_t buffer[1024];
	int got_to_recv = 0;
	int status;
	int transferred_local;
	if (!transferred)
	{
		transferred = &transferred_local;
	}
	status = libusb_bulk_transfer(devh, 0x01, command, commandlen, transferred, WCHTIMEOUT);
	if (status)
	{
		goto sendfail;
	}
	got_to_recv = 1;
	if (!reply)
	{
		reply = buffer;
		replymax = sizeof(buffer);
	}

	//	printf("wch_link_command send (%d)", commandlen); for(int i = 0; i< commandlen; printf(" %02x",command[i++]));
	// printf("\n");

	status = libusb_bulk_transfer(devh, 0x81, reply, replymax, transferred, WCHTIMEOUT);

	//	printf("wch_link_command reply (%d)", *transferred); for(int i = 0; i< *transferred; printf("
	//%02x",reply[i++])); printf("\n");

	if (status)
	{
		goto sendfail;
	}
	return;
sendfail:
	fprintf(stderr, "Error sending WCH command (%s): ", got_to_recv ? "on recv" : "on send");
	int i;
	for (i = 0; i < commandlen; i++)
	{
		printf("%02x ", command[i]);
	}
	printf("\n");
	exit(status);
}

/* Like wch_link_command, but returns the libusb status instead of exiting. */
static int wch_link_command_try(
    libusb_device_handle *devh, const void *command_v, int commandlen, int *transferred, uint8_t *reply, int replymax
)
{
	uint8_t *command = (uint8_t *)command_v;
	uint8_t buffer[1024];
	int status;
	int transferred_local;
	if (!transferred)
		transferred = &transferred_local;
	status = libusb_bulk_transfer(devh, 0x01, command, commandlen, transferred, WCHTIMEOUT);
	if (status)
		return status;
	if (!reply)
	{
		reply = buffer;
		replymax = sizeof(buffer);
	}
	status = libusb_bulk_transfer(devh, 0x81, reply, replymax, transferred, WCHTIMEOUT);
	return status;
}

static void wch_link_multicommands(libusb_device_handle *devh, int nrcommands, ...)
{
	int i;
	va_list argp;
	va_start(argp, nrcommands);
	for (i = 0; i < nrcommands; i++)
	{
		int clen = va_arg(argp, int);
		wch_link_command(devh, va_arg(argp, char *), clen, 0, 0, 0);
	}
	va_end(argp);
}

static inline libusb_device_handle *wch_link_base_setup(int inhibit_startup)
{
	libusb_context *ctx = 0;
	int status;
	status = libusb_init(&ctx);
	if (status < 0)
	{
		fprintf(stderr, "Error: libusb_init_context() returned %d\n", status);
		exit(status);
	}

	libusb_device **list;
	ssize_t cnt = libusb_get_device_list(ctx, &list);
	ssize_t i = 0;

	libusb_device *found = NULL;
	libusb_device *found_arm_programmer = NULL;
	libusb_device *found_programmer_in_iap = NULL;

	for (i = 0; i < cnt; i++)
	{
		libusb_device *device = list[i];
		struct libusb_device_descriptor desc;
		int r = libusb_get_device_descriptor(device, &desc);
		if (r == 0 && desc.idVendor == 0x1a86 && desc.idProduct == 0x8010)
		{
			found = device;
		}
		if (r == 0 && desc.idVendor == 0x1a86 && desc.idProduct == 0x8012)
		{
			found_arm_programmer = device;
		}
		if (r == 0 && desc.idVendor == 0x4348 && desc.idProduct == 0x55e0)
		{
			found_programmer_in_iap = device;
		}
	}

	if (!found)
	{
		// On a lark see if we have a programmer which got stuck in IAP mode.

		if (found_arm_programmer)
		{
			fprintf(
			    stderr, "Warning: found at least one WCH-LinkE in ARM programming mode. Attempting automatic "
			    "switch to RISC-V.  Will need a to re-attempt.\n"
			);
			fprintf(
			    stderr, "For more information, you may need to change it to RISC-V mode as per "
			    "https://github.com/cnlohr/ch32v003fun/issues/227\n"
			);

			// Just in case we got stuck in IAP mode, try sending 0x83 to eject.
			libusb_device_handle *devh = 0;
			status = libusb_open(found_arm_programmer, &devh);
			if (status)
			{
				fprintf(stderr, "Found programmer in ARM mode, but couldn't open it.\n");
				exit(-10);
			}

			// https://github.com/wagiminator/MCU-Flash-Tools/blob/main/rvmode.py
			uint8_t rbuff[4] = {0x81, 0xff, 0x01, 0x52};
			int transferred = 0;
			libusb_bulk_transfer(devh, 0x02, rbuff, 4, &transferred, 1);
			fprintf(stderr, "RISC-V command sent (%d)\n", transferred);
			exit(-3);
		}


		if (found_programmer_in_iap)
		{
			// Just in case we got stuck in IAP mode, try sending 0x83 to eject.
			fprintf(stderr, "Found programmer in IAP mode. Attempting to eject it out of IAP.\n");
			libusb_device_handle *devh = 0;
			status = libusb_open(found_programmer_in_iap, &devh);
			if (status)
			{
				fprintf(stderr, "Found programmer in IAP mode, but couldn't open it.\n");
				exit(-10);
			}
			uint8_t rbuff[4];
			int transferred = 0;
			rbuff[0] = 0x83;
			libusb_bulk_transfer(devh, 0x02, rbuff, 1, &transferred, 1);
			fprintf(stderr, "Eject command sent (%d)\n", transferred);
			exit(-3);
		}

		return 0;
	}

	libusb_device_handle *devh;
	status = libusb_open(found, &devh);
	if (status)
	{
		fprintf(stderr, "Error: couldn't open wch link device (libusb_open() = %d)\n", status);
		return 0;
	}

	WCHCHECK(libusb_claim_interface(devh, 0));

	uint8_t rbuff[1024];
	int transferred;
	libusb_bulk_transfer(
	    devh, 0x81, rbuff, 1024, &transferred, 1
	); // Clear out any pending transfers.  Don't wait though.

	return devh;
}

// DMI_OP decyphered From
// https://github.com/karlp/openocd-hacks/blob/27af153d4a373f29ad93dab28a01baffb7894363/src/jtag/drivers/wlink.c Thanks,
// CW2 for pointing this out.  See DMI_OP for more info.
int LEWriteReg32(void *dev, uint8_t reg_7_bit, uint32_t command)
{
	libusb_device_handle *devh = ((struct LinkEProgrammerStruct *)dev)->devh;

	const uint8_t iOP = 2; // op 2 = write
	uint8_t req[] =
	{
		0x81,
		0x08,
		0x06,
		reg_7_bit,
		(command >> 24) & 0xff,
		(command >> 16) & 0xff,
		(command >> 8) & 0xff,
		(command >> 0) & 0xff,
		iOP
	};

	uint8_t resp[128];
	int resplen;
	wch_link_command(devh, req, sizeof(req), &resplen, resp, sizeof(resp));
	if (resplen != 9 || resp[8] == 0x02 || resp[8] == 0x03) //|| resp[3] != reg_7_bit )
	{
		fprintf(stderr, "Error setting write reg. Tell cnlohr. Maybe we should allow retries here?\n");
		fprintf(stderr, "RR: %d :", resplen);
		int i;
		for (i = 0; i < resplen; i++)
		{
			fprintf(stderr, "%02x ", resp[i]);
		}
		fprintf(stderr, "\n");
	}
	return 0;
}

int LEReadReg32(void *dev, uint8_t reg_7_bit, uint32_t *commandresp)
{
	libusb_device_handle *devh = ((struct LinkEProgrammerStruct *)dev)->devh;
	const uint8_t iOP = 1; // op 1 = read
	uint32_t transferred;
	uint8_t rbuff[128] = {0};
	uint8_t req[] = {0x81, 0x08, 0x06, reg_7_bit, 0, 0, 0, 0, iOP};
	wch_link_command(devh, req, sizeof(req), (int *)&transferred, rbuff, sizeof(rbuff));
	*commandresp = (rbuff[4] << 24) | (rbuff[5] << 16) | (rbuff[6] << 8) | (rbuff[7] << 0);
	if (transferred != 9 || rbuff[8] == 0x02 || rbuff[8] == 0x03) //|| rbuff[3] != reg_7_bit )
	{
		fprintf(stderr, "Error setting write reg. Tell cnlohr. Maybe we should allow retries here?\n");
		fprintf(stderr, "RR: %d :", transferred);
		int i;
		for (i = 0; i < transferred; i++)
		{
			fprintf(stderr, "%02x ", rbuff[i]);
		}
		fprintf(stderr, "\n");
	}
	/*
	printf( "RR: %d :", transferred );
	int i;
	for( i = 0; i < transferred; i++ )
	{
	    printf( "%02x ", rbuff[i] );
	}
	printf( "\n" );
	*/

	return 0;
}

int LEFlushLLCommands(void *dev)
{
	return 0;
}

static int LESetupInterface(void *d)
{
	libusb_device_handle *dev = ((struct LinkEProgrammerStruct *)d)->devh;
	struct InternalState *iss = (struct InternalState *)(((struct ProgrammerStructBase *)d)->internal);
	uint8_t rbuff[1024];
	uint32_t transferred = 0;

	/* A pin reset must not connect. The failed 0x02 on a protected,
	 * running part takes a debug hold that survives until power is cycled. */
	if (iss->hardware_reset)
	{
		fprintf(stderr, "NRST reset only; not attaching\n");
		iss->skip_debug_on_exit = 1;
		return 0;
	}

	/* 0x03 is the debug hold. On a protected part it survives 0xff until
	 * power is cycled, so a pin reset must not take it. */
	if (!iss->avoid_debug_hold)
		wch_link_command(dev, "\x81\x0d\x01\x03", 4, (int *)&transferred, rbuff, 1024);

	// Place part into reset.
	wch_link_command(
	    dev, "\x81\x0d\x01\x01", 4, (int *)&transferred, rbuff, 1024
	); // Reply is: "\x82\x0d\x04\x02\x08\x02\x00"
	switch (rbuff[5])
	{
	case 1:
		fprintf(stderr, "WCH Programmer is CH549 version %d.%d\n", rbuff[3], rbuff[4]);
		break;
	case 2:
		fprintf(stderr, "WCH Programmer is CH32V307 version %d.%d\n", rbuff[3], rbuff[4]);
		break;
	case 3:
		fprintf(stderr, "WCH Programmer is CH32V203 version %d.%d\n", rbuff[3], rbuff[4]);
		break;
	case 4:
		fprintf(stderr, "WCH Programmer is LinkB version %d.%d\n", rbuff[3], rbuff[4]);
		break;
	case 18:
		fprintf(stderr, "WCH Programmer is LinkE version %d.%d\n", rbuff[3], rbuff[4]);
		break;
	default:
		fprintf(stderr, "Unknown WCH Programmer %02x (Ver %d.%d)\n", rbuff[5], rbuff[3], rbuff[4]);
		break;
	}

	// TODO: What in the world is this?  It doesn't appear to be needed.
	wch_link_command(dev, "\x81\x0c\x02\x09\x01", 5, 0, 0, 0); // Reply is: 820c0101

	// Note from further debugging:
	// My capture differs in this case: \x05 instead of \x09 -> But does not seem to be needed
	// wch_link_command( dev, "\x81\x0c\x02\x05\x01", 5, 0, 0, 0 ); //Reply is: 820c0101

	// This puts the processor on hold to allow the debugger to run.
	int already_tried_reset = 0;
	do
	{
		wch_link_command(
		    dev, "\x81\x0d\x01\x02", 4, (int *)&transferred, rbuff, 1024
		); // Reply: Ignored, 820d050900300500
		if (rbuff[0] == 0x81 && rbuff[1] == 0x55 && rbuff[2] == 0x01) // && rbuff[3] == 0x01 )
		{
			// The following code may try to execute a few times to get the processor to actually reset.
			// This code could likely be much better.

			/* SWD is off because the protected image is running. The
			 * hold-and-retry below parks the hart, and 0xff then leaves
			 * that park until power is cycled. A reset only pulses NRST. */
			if (iss->hardware_reset)
			{
				fprintf(stderr, "SWD is off. Pulsing NRST without attaching.\n");
				iss->skip_debug_on_exit = 1;
				return 0;
			}

			/* Protected firmware has turned SWD off and is running.
			 * The hold-and-retry below would park it until power is cycled. */
			if (iss->protect_run)
			{
				fprintf(stderr, "SWD is off. Chip is running; not breaking in.\n");
				iss->leave_running = 1;
				return 0;
			}

			if (iss->no_attach_reset)
			{
				if (iss->swd_probe)
				{
					fprintf(
					    stderr,
					    "SWD did not respond (%d = [%02x %02x %02x %02x]). Attach failed; the part was not reset.\n",
					    transferred, rbuff[0], rbuff[1], rbuff[2], rbuff[3]
					);
				}
				else
				{
					uint8_t prbuf[16];
					int pn = 0;
					fprintf(
					    stderr,
					    "SWD not connected (%d = [%02x %02x %02x %02x]). Not resetting the part.\n",
					    transferred, rbuff[0], rbuff[1], rbuff[2], rbuff[3]
					);
					if (wch_link_command_try(dev, "\x81\x06\x01\x01", 4, &pn, prbuf, sizeof(prbuf)) == 0 && pn >= 4)
					{
						fprintf(stderr, "Read protection: %s\n", prbuf[3] == 0x01 ? "enabled" : "disabled");
					}
					fprintf(stderr, "Status does not break in over reset.\n");
				}
				iss->skip_debug_on_exit = 1;
				return -1;
			}

			fprintf(
			    stderr,
			    "link error, nothing connected to linker (%d = [%02x %02x %02x %02x]).  Trying to put processor in "
			    "hold and retrying.\n",
			    transferred, rbuff[0], rbuff[1], rbuff[2], rbuff[3]
			);

			/* 0x03 (debug hold) survives a failed attach and the hart
			 * stays stopped until power is cycled. Pulse NRST only. */
			if (already_tried_reset >= 4)
			{
				fprintf(stderr,
				        "Could not attach (read protection, or SWD turned off after reset). "
				        "Releasing reset.\n");
				return -1;
			}

			wch_link_multicommands(
			    (libusb_device_handle *)dev, 1, 4, "\x81\x0d\x01\x13"
			); // Try forcing reset line low.
			MCF.DelayUS(iss, 5000);

			wch_link_multicommands((libusb_device_handle *)dev, 1, 4, "\x81\x0d\x01\x14"); // Release reset line.
			wch_link_multicommands(
			    (libusb_device_handle *)dev, 3, 4, "\x81\x0b\x01\x01", 4, "\x81\x0d\x01\x02", 4, "\x81\x0d\x01\xff"
			);
			already_tried_reset++;
		}
		else
		{
			break;
		}
	}
	while (1);

	if (rbuff[3] == 0x08 || rbuff[3] > 0x09)
	{
		fprintf(stderr, "Chip Type unknown. Aborting...\n");
		return -1;
	}

	enum RiscVChip chip = (enum RiscVChip)rbuff[3];
	printChipInfo(chip);

	int result = checkChip(chip);
	if (result == 1) // Using blob write
	{
		fprintf(stderr, "Using binary blob write for operation.\n");
		MCF.WriteBinaryBlob = LEWriteBinaryBlob;

		iss->sector_size = 256;

		if (!iss->hardware_reset && !iss->avoid_debug_hold)
			wch_link_command(
			    dev, "\x81\x0d\x01\x03", 4, (int *)&transferred, rbuff, 1024
			);
	}
	else if (result < 0)
	{
		fprintf(stderr, "Chip type not supported. Aborting...\n");
		return -1;
	}

	iss->target_chip_type = chip;

	/* haltreq here is what sticks across disconnect when readout
	 * protection is on. A hardware reset must not set it. */
	if (!iss->hardware_reset && !iss->avoid_debug_hold)
	{
		// For some reason, if we don't do this sometimes the programmer starts in a hosey mode.
		MCF.WriteReg32(d, DMCONTROL, 0x80000001); // Make the debug module work properly.
		MCF.WriteReg32(d, DMCONTROL, 0x80000001); // Initiate a halt request.
		MCF.WriteReg32(d, DMCONTROL, 0x80000001); // No, really make sure.
		MCF.WriteReg32(d, DMABSTRACTCS, 0x00000700); // Ignore any pending errors.
		MCF.WriteReg32(d, DMABSTRACTAUTO, 0);
		MCF.WriteReg32(d, DMCOMMAND, 0x00221000); // Read x0 (Null command) with nopostexec (to fix v307 read issues)

		int r = 0;

		r |= MCF.WaitForDoneOp(d, 0);
		if (r)
		{
			fprintf(stderr, "Fault on setup\n");
		}
		else
		{
			fprintf(stderr, "Setup success\n");
		}
	}
	else
	{
		iss->reset_attached = 1;
		fprintf(stderr, "Setup success\n");
	}

	// This puts the processor on hold to allow the debugger to run.
	// Recommended to switch to 05 from 09 by Alexander M
	//	wch_link_command( dev, "\x81\x11\x01\x09", 4, (int*)&transferred, rbuff, 1024 ); // Reply: Chip ID + Other data
	//(see below)
	wch_link_command(
	    dev, "\x81\x11\x01\x05", 4, (int *)&transferred, rbuff, 1024
	); // Reply: Chip ID + Other data (see below)

	if (transferred != 20)
	{
		fprintf(stderr, "Error: could not get part status\n");
		return -1;
	}
	fprintf(stderr, "Flash Storage: %d kB\n", (rbuff[2] << 8) | rbuff[3]); // Is this Flash size?
	fprintf(
	    stderr, "Part UUID    : %02x-%02x-%02x-%02x-%02x-%02x-%02x-%02x\n", rbuff[4], rbuff[5], rbuff[6], rbuff[7],
	    rbuff[8], rbuff[9], rbuff[10], rbuff[11]
	);
	fprintf(stderr, "PFlags       : %02x-%02x-%02x-%02x\n", rbuff[12], rbuff[13], rbuff[14], rbuff[15]);
	fprintf(stderr, "Part Type (B): %02x-%02x-%02x-%02x\n", rbuff[16], rbuff[17], rbuff[18], rbuff[19]);

	// Check for read protection
	wch_link_command(dev, "\x81\x06\x01\x01", 4, (int *)&transferred, rbuff, 1024);
	if (transferred != 4)
	{
		fprintf(stderr, "Error: could not get read protection status\n");
		return -1;
	}

	if (rbuff[3] == 0x01)
	{
		iss->read_protected = 1;
		fprintf(stderr, "Read protection: enabled\n");
	}
	else
	{
		fprintf(stderr, "Read protection: disabled\n");
	}

	iss->flash_size = ((rbuff[2] << 8) | rbuff[3]) * 1024;

	return 0;
}

static int LEControl3v3(void *d, int bOn)
{
	libusb_device_handle *dev = ((struct LinkEProgrammerStruct *)d)->devh;

	if (bOn)
	{
		wch_link_command((libusb_device_handle *)dev, "\x81\x0d\x01\x09", 4, 0, 0, 0);
	}
	else
	{
		wch_link_command((libusb_device_handle *)dev, "\x81\x0d\x01\x0a", 4, 0, 0, 0);
	}
	return 0;
}

static int LEControl5v(void *d, int bOn)
{
	libusb_device_handle *dev = ((struct LinkEProgrammerStruct *)d)->devh;

	if (bOn)
	{
		wch_link_command((libusb_device_handle *)dev, "\x81\x0d\x01\x0b", 4, 0, 0, 0);
	}
	else
	{
		wch_link_command((libusb_device_handle *)dev, "\x81\x0d\x01\x0c", 4, 0, 0, 0);
	}
	return 0;
}

static int LEUnbrick(void *d)
{
	printf("Sending unbrick\n");
	libusb_device_handle *dev = ((struct LinkEProgrammerStruct *)d)->devh;
	wch_link_command((libusb_device_handle *)dev, "\x81\x0d\x01\x0f\x09", 5, 0, 0, 0);
	printf("Done unbrick\n");
	return 0;
}


static int LEConfigureNRSTAsGPIO(void *d, int one_if_yes_gpio)
{
	libusb_device_handle *dev = ((struct LinkEProgrammerStruct *)d)->devh;

	if (one_if_yes_gpio)
	{
		wch_link_multicommands(
		    (libusb_device_handle *)dev, 2, 11, "\x81\x06\x08\x02\xff\xff\xff\xff\xff\xff\xff", 4, "\x81\x0b\x01\x01"
		);
	}
	else
	{
		wch_link_multicommands(
		    (libusb_device_handle *)dev, 2, 11, "\x81\x06\x08\x02\xf7\xff\xff\xff\xff\xff\xff", 4, "\x81\x0b\x01\x01"
		);
	}
	return 0;
}

static int LEConfigureReadProtection(void *d, int one_if_yes_protect)
{
	libusb_device_handle *dev = ((struct LinkEProgrammerStruct *)d)->devh;

	if (one_if_yes_protect)
	{
		uint8_t rbuff[64];
		int transferred = 0;
		int status;
		uint8_t user = 0x7f; /* 96K split, if the option word cannot be read */
		uint32_t opt = 0;

		/* USER is the low byte of the high halfword at 0x1FFFF800.
		 * Keep it: the old 0xf7 write replaced the RAM split. */
		if (MCF.ReadWord && MCF.ReadWord(d, 0x1FFFF800, &opt) == 0)
			user = (uint8_t)((opt >> 16) & 0xff);

		fprintf(stderr, "Enabling read protection (USER=0x%02x)\n", user);

		/* 0x06/0x01/0x03 is acked by LinkE 2.22 and leaves the flag at
		 * 0x02. The option-byte write is what programs readout protection.
		 * Do it on a fresh attach while the hart is still halted, then
		 * quit-reset so the new option byte is loaded before the check. */
		if (wch_link_command_try(dev, "\x81\x0d\x01\xff", 4, 0, 0, 0) ||
		    wch_link_command_try(dev, "\x81\x0d\x01\x02", 4, 0, 0, 0))
		{
			fprintf(stderr, "Error: could not reattach before protect\n");
			wch_link_command_try(dev, "\x81\x0d\x01\x02", 4, 0, 0, 0);
			return -1;
		}

		wch_link_command_try(dev, "\x81\x11\x01\x09", 4, 0, 0, 0);
		{
			uint8_t cmd[11] = {0x81, 0x06, 0x08, 0x03, user, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
			status = wch_link_command_try(dev, cmd, sizeof(cmd), &transferred, rbuff, sizeof(rbuff));
		}
		if (status)
		{
			fprintf(stderr, "Error: protect command failed (%d)\n", status);
			wch_link_command_try(dev, "\x81\x0d\x01\x02", 4, 0, 0, 0);
			return -1;
		}
		/* Release the hart before any reset. Readout protection is not
		 * active until that reset, so this resume still works. */
		if (MCF.WriteReg32)
		{
			fprintf(stderr, "Releasing halt before disconnect\n");
			MCF.WriteReg32(d, DMCONTROL, 0x00000001);
			MCF.WriteReg32(d, DMCONTROL, 0x40000001);
			if (MCF.DelayUS)
				MCF.DelayUS(d, 50000);
			MCF.WriteReg32(d, DMCONTROL, 0x00000001);
		}
		fprintf(stderr, "Read protection programmed\n");
	}
	else
	{
		struct InternalState *iss = (struct InternalState *)(((struct ProgrammerStructBase *)d)->internal);
		uint8_t rbuff[64];
		int transferred = 0;
		int status;
		uint8_t user = 0x7f;

		/* -K sets this. 0xf7 is not a RAM-split byte, so the old write
		 * never cleared RDPR. */
		switch (iss->wanted_ram_split)
		{
		case 32: user = 0xff; break;
		case 64: user = 0xbf; break;
		case 96: user = 0x7f; break;
		case 128: user = 0x3f; break;
		case 192: user = 0xdf; break;
		default: break;
		}

		fprintf(stderr, "Disabling read protection (USER=0x%02x). This mass-erases application flash.\n", user);

		if (wch_link_command_try(dev, "\x81\x0d\x01\xff", 4, 0, 0, 0) ||
		    wch_link_command_try(dev, "\x81\x0d\x01\x02", 4, 0, 0, 0))
		{
			fprintf(stderr, "Error: could not reattach before unprotect\n");
			wch_link_command_try(dev, "\x81\x0d\x01\x02", 4, 0, 0, 0);
			return -1;
		}

		/* Short 0x02 is "set read-unprotected". The 11-byte form keeps USER. */
		status = wch_link_command_try(dev, "\x81\x06\x01\x02", 4, &transferred, rbuff, sizeof(rbuff));
		if (!status)
		{
			wch_link_command_try(dev, "\x81\x0b\x01\x01", 4, 0, 0, 0);
			if (MCF.DelayUS)
				MCF.DelayUS(d, 300000);
		}
		if (wch_link_command_try(dev, "\x81\x0d\x01\xff", 4, 0, 0, 0) ||
		    wch_link_command_try(dev, "\x81\x0d\x01\x02", 4, 0, 0, 0) ||
		    wch_link_command_try(dev, "\x81\x06\x01\x01", 4, &transferred, rbuff, sizeof(rbuff)) ||
		    transferred < 4 || rbuff[3] == 0x01)
		{
			uint8_t cmd[11] = {0x81, 0x06, 0x08, 0x02, user, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
			fprintf(stderr, "Short unprotect left protection enabled, trying option-byte form\n");
			status = wch_link_command_try(dev, cmd, sizeof(cmd), &transferred, rbuff, sizeof(rbuff));
			if (status)
			{
				fprintf(stderr, "Error: unprotect command failed (%d)\n", status);
				return -1;
			}
			if (wch_link_command_try(dev, "\x81\x0b\x01\x01", 4, 0, 0, 0))
			{
				fprintf(stderr, "Error: quit-reset after unprotect failed\n");
				return -1;
			}
			if (MCF.DelayUS)
				MCF.DelayUS(d, 300000);
			if (wch_link_command_try(dev, "\x81\x0d\x01\xff", 4, 0, 0, 0) ||
			    wch_link_command_try(dev, "\x81\x0d\x01\x02", 4, 0, 0, 0) ||
			    wch_link_command_try(dev, "\x81\x06\x01\x01", 4, &transferred, rbuff, sizeof(rbuff)) ||
			    transferred < 4)
			{
				fprintf(stderr, "Error: could not re-read protection status\n");
				return -1;
			}
		}
		if (rbuff[3] == 0x01)
		{
			fprintf(stderr, "Error: readout protection is still enabled\n");
			return -1;
		}
		fprintf(stderr, "Read protection: disabled\n");
	}
	return 0;
}

int LEExit(void *d)
{
	libusb_device_handle *dev = ((struct LinkEProgrammerStruct *)d)->devh;
	struct InternalState *iss = (struct InternalState *)(((struct ProgrammerStructBase *)d)->internal);

	if (iss && iss->leave_running)
		return 0;

	/* make protect only. Debugger is dropped while the hart is running
	 * and readout protection is not latched yet. The pin pulse then
	 * loads the option byte with no debug hold in place. */
	if (iss && iss->pin_reset_after_detach)
	{
		fprintf(stderr, "Ending debug session\n");
		wch_link_command((libusb_device_handle *)dev, "\x81\x0d\x01\xff", 4, 0, 0, 0);
		if (MCF.DelayUS)
			MCF.DelayUS(d, 50000);
		fprintf(stderr, "Pulsing NRST\n");
		wch_link_command((libusb_device_handle *)dev, "\x81\x0d\x01\x13", 4, 0, 0, 0);
		if (MCF.DelayUS)
			MCF.DelayUS(d, 50000);
		wch_link_command((libusb_device_handle *)dev, "\x81\x0d\x01\x14", 4, 0, 0, 0);
		if (MCF.DelayUS)
			MCF.DelayUS(d, 50000);
		return 0;
	}

	/* make reset only. Never attached. Pin pulse, nothing else. */
	if (iss && iss->hardware_reset)
	{
		fprintf(stderr, "Pulsing NRST\n");
		wch_link_command((libusb_device_handle *)dev, "\x81\x0d\x01\x14", 4, 0, 0, 0);
		if (MCF.DelayUS)
			MCF.DelayUS(d, 20000);
		wch_link_command((libusb_device_handle *)dev, "\x81\x0d\x01\x13", 4, 0, 0, 0);
		if (MCF.DelayUS)
			MCF.DelayUS(d, 50000);
		wch_link_command((libusb_device_handle *)dev, "\x81\x0d\x01\x14", 4, 0, 0, 0);
		if (MCF.DelayUS)
			MCF.DelayUS(d, 50000);
		return 0;
	}

	/* Reset (-b) and protect (-P) quit reset before disconnect. After -P
	 * the debug module is locked, so a DMI resume cannot clear the hold. */
	if (iss && iss->release_from_reset)
	{
		fprintf(stderr, "Releasing processor from reset\n");
		if (!(iss->skip_debug_on_exit) && MCF.WriteReg32)
		{
			MCF.WriteReg32(d, DMCONTROL, 0x00000001); /* dmactive; ndmreset and haltreq clear */
		}
		/* WCH quit-reset (0x0b/0x01). The probe finishes releasing NRST during this delay. */
		wch_link_command((libusb_device_handle *)dev, "\x81\x0b\x01\x01", 4, 0, 0, 0);
		if (MCF.DelayUS)
		{
			MCF.DelayUS(d, 300000);
		}
	}

	wch_link_command((libusb_device_handle *)dev, "\x81\x0d\x01\x14", 4, 0, 0, 0);
	if (!(iss && iss->skip_debug_on_exit) && MCF.WriteReg32)
	{
		MCF.WriteReg32(d, DMCONTROL, 0x00000001); /* dmactive, haltreq clear */
		MCF.WriteReg32(d, DMCONTROL, 0x40000001); /* resumereq */
		MCF.WriteReg32(d, DMCONTROL, 0x00000001);
	}
	wch_link_command((libusb_device_handle *)dev, "\x81\x0d\x01\xff", 4, 0, 0, 0);
	return 0;
}

void *TryInit_WCHLinkE()
{
	libusb_device_handle *wch_linke_devh;
	wch_linke_devh = wch_link_base_setup(0);
	if (!wch_linke_devh)
	{
		return 0;
	}

	struct LinkEProgrammerStruct *ret = malloc(sizeof(struct LinkEProgrammerStruct));
	memset(ret, 0, sizeof(*ret));
	ret->devh = wch_linke_devh;
	ret->lasthaltmode = 0;

	MCF.ReadReg32 = LEReadReg32;
	MCF.WriteReg32 = LEWriteReg32;
	MCF.FlushLLCommands = LEFlushLLCommands;

	MCF.SetupInterface = LESetupInterface;
	MCF.Control3v3 = LEControl3v3;
	MCF.Control5v = LEControl5v;
	MCF.Unbrick = LEUnbrick;
	MCF.ConfigureNRSTAsGPIO = LEConfigureNRSTAsGPIO;
	MCF.ConfigureReadProtection = LEConfigureReadProtection;

	MCF.Exit = LEExit;
	return ret;
};


#if 1

// Flash Bootloader for V20x and V30x series MCUs

const uint8_t *bootloader_v1 = (const uint8_t *)"\x93\x77\x15\x00\x41\x11\x99\xCF\xB7\x06\x67\x45\xB7\x27\x02\x40"
                               "\x93\x86\x36\x12\x37\x97\xEF\xCD\xD4\xC3\x13\x07\xB7\x9A\xD8\xC3"
                               "\xD4\xD3\xD8\xD3\x93\x77\x25\x00\x9D\xC7\xB7\x27\x02\x40\x98\x4B"
                               "\xAD\x66\x37\x38\x00\x40\x13\x67\x47\x00\x98\xCB\x98\x4B\x93\x86"
                               "\xA6\xAA\x13\x67\x07\x04\x98\xCB\xD8\x47\x05\x8B\x63\x1F\x07\x10"
                               "\x98\x4B\x6D\x9B\x98\xCB\x93\x77\x45\x00\xA9\xCB\x93\x07\xF6\x07"
                               "\x9D\x83\x2E\xC0\x2D\x68\x81\x76\x3E\xC4\xB7\x08\x02\x00\xB7\x27"
                               "\x02\x40\x37\x33\x00\x40\x13\x08\xA8\xAA\xFD\x16\x98\x4B\x33\x67"
                               "\x17\x01\x98\xCB\x02\x47\xD8\xCB\x98\x4B\x13\x67\x07\x04\x98\xCB"
                               "\xD8\x47\x05\x8B\x71\xEF\x98\x4B\x75\x8F\x98\xCB\x02\x47\x13\x07"
                               "\x07\x08\x3A\xC0\x22\x47\x7D\x17\x3A\xC4\x69\xFB\x93\x77\x85\x00"
                               "\xED\xC3\x93\x07\xF6\x07\x2E\xC0\x9D\x83\x37\x27\x02\x40\x3E\xC4"
                               "\x1C\x4B\xC1\x66\x37\x08\x08\x00\xD5\x8F\x1C\xCB\xA1\x48\x37\x17"
                               "\x00\x20\xB7\x27\x02\x40\x37\x03\x04\x00\x94\x4B\xB3\xE6\x06\x01"
                               "\x94\xCB\xD4\x47\x85\x8A\xF5\xFE\x82\x46\x3A\x8E\x36\xC2\x46\xC6"
                               "\x92\x46\x83\x2E\x07\x00\x41\x07\x23\xA0\xD6\x01\x92\x46\x83\x2E"
                               "\x47\xFF\x23\xA2\xD6\x01\x92\x46\x83\x2E\x87\xFF\x23\xA4\xD6\x01"
                               "\x92\x46\x03\x2E\xCE\x00\x23\xA6\xC6\x01\x94\x4B\xB3\xE6\x66\x00"
                               "\x94\xCB\xD4\x47\x85\x8A\xF5\xFE\x92\x46\x3A\x8E\xC1\x06\x36\xC2"
                               "\xB2\x46\xFD\x16\x36\xC6\xCD\xFE\x82\x46\xD4\xCB\x94\x4B\x93\xE6"
                               "\x06\x04\x94\xCB\xD4\x47\x85\x8A\xF5\xFE\xD4\x47\xD1\x8A\x85\xC6"
                               "\xD8\x47\xB7\x06\xF3\xFF\xFD\x16\x13\x67\x47\x01\xD8\xC7\x98\x4B"
                               "\x21\x45\x75\x8F\x98\xCB\x41\x01\x02\x90\x23\x20\xD8\x00\xE9\xBD"
                               "\x23\x20\x03\x01\x31\xBF\x82\x46\x93\x86\x06\x08\x36\xC0\xA2\x46"
                               "\xFD\x16\x36\xC4\xB9\xFA\x98\x4B\xB7\x06\xF3\xFF\xFD\x16\x75\x8F"
                               "\x98\xCB\x41\x89\x15\xC9\x2E\xC0\x0D\x06\x02\xC4\x09\x82\x32\xC6"
                               "\xB7\x17\x00\x20\x98\x43\x13\x86\x47\x00\xA2\x47\x82\x46\x8A\x07"
                               "\xB6\x97\x9C\x43\x63\x1C\xF7\x00\xA2\x47\x85\x07\x3E\xC4\xA2\x46"
                               "\x32\x47\xB2\x87\xE3\xE0\xE6\xFE\x01\x45\x71\xBF\x41\x45\x61\xBF"
                               "\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff"
                               "\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff"
                               "\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff";

const uint8_t *bootloader_v2 = (const uint8_t *)"\x93\x77\x15\x00\x41\x11\x99\xcf\xb7\x06\x67\x45\xb7\x27\x02\x40"
                               "\x93\x86\x36\x12\x37\x97\xef\xcd\xd4\xc3\x13\x07\xb7\x9a\xd8\xc3"
                               "\xd4\xd3\xd8\xd3\x93\x77\x25\x00\x95\xc7\xb7\x27\x02\x40\x98\x4b"
                               "\xad\x66\x37\x38\x00\x40\x13\x67\x47\x00\x98\xcb\x98\x4b\x93\x86"
                               "\xa6\xaa\x13\x67\x07\x04\x98\xcb\xd8\x47\x05\x8b\x61\xeb\x98\x4b"
                               "\x6d\x9b\x98\xcb\x93\x77\x45\x00\xa9\xcb\x93\x07\xf6\x0f\xa1\x83"
                               "\x2e\xc0\x2d\x68\x81\x76\x3e\xc4\xb7\x08\x02\x00\xb7\x27\x02\x40"
                               "\x37\x33\x00\x40\x13\x08\xa8\xaa\xfd\x16\x98\x4b\x33\x67\x17\x01"
                               "\x98\xcb\x02\x47\xd8\xcb\x98\x4b\x13\x67\x07\x04\x98\xcb\xd8\x47"
                               "\x05\x8b\x41\xeb\x98\x4b\x75\x8f\x98\xcb\x02\x47\x13\x07\x07\x10"
                               "\x3a\xc0\x22\x47\x7d\x17\x3a\xc4\x69\xfb\x93\x77\x85\x00\xd5\xcb"
                               "\x93\x07\xf6\x0f\x2e\xc0\xa1\x83\x3e\xc4\x37\x27\x02\x40\x1c\x4b"
                               "\xc1\x66\x41\x68\xd5\x8f\x1c\xcb\xb7\x16\x00\x20\xb7\x27\x02\x40"
                               "\x93\x08\x00\x04\x37\x03\x20\x00\x98\x4b\x33\x67\x07\x01\x98\xcb"
                               "\xd8\x47\x05\x8b\x75\xff\x02\x47\x3a\xc2\x46\xc6\x32\x47\x0d\xef"
                               "\x98\x4b\x33\x67\x67\x00\x98\xcb\xd8\x47\x05\x8b\x75\xff\xd8\x47"
                               "\x41\x8b\x39\xc3\xd8\x47\xc1\x76\xfd\x16\x13\x67\x07\x01\xd8\xc7"
                               "\x98\x4b\x21\x45\x75\x8f\x98\xcb\x41\x01\x02\x90\x23\x20\xd8\x00"
                               "\x25\xb7\x23\x20\x03\x01\xa5\xb7\x12\x47\x13\x8e\x46\x00\x94\x42"
                               "\x14\xc3\x12\x47\x11\x07\x3a\xc2\x32\x47\x7d\x17\x3a\xc6\xd8\x47"
                               "\x09\x8b\x75\xff\xf2\x86\x5d\xb7\x02\x47\x13\x07\x07\x10\x3a\xc0"
                               "\x22\x47\x7d\x17\x3a\xc4\x49\xf3\x98\x4b\xc1\x76\xfd\x16\x75\x8f"
                               "\x98\xcb\x41\x89\x15\xc9\x2e\xc0\x0d\x06\x02\xc4\x09\x82\x32\xc6"
                               "\xb7\x17\x00\x20\x98\x43\x13\x86\x47\x00\xa2\x47\x82\x46\x8a\x07"
                               "\xb6\x97\x9c\x43\x63\x1c\xf7\x00\xa2\x47\x85\x07\x3e\xc4\xa2\x46"
                               "\x32\x47\xb2\x87\xe3\xe0\xe6\xfe\x01\x45\xbd\xbf\x41\x45\xad\xbf"
                               "\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff"
                               "\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff"
                               "\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff"
                               "\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff"
                               "\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff"
                               "\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff";

int bootloader_len = 512;
#endif

static const uint8_t *GetFlashLoader(enum RiscVChip chip)
{
	switch (chip)
	{
	case CHIP_CH32V10x:
		return bootloader_v1;
	case CHIP_CH32V20x:
	case CHIP_CH32V30x:
	default:
		return bootloader_v2;
	}
}

static int InternalLinkEHaltMode(void *d, int mode)
{
	libusb_device_handle *dev = ((struct LinkEProgrammerStruct *)d)->devh;
	if (mode == ((struct LinkEProgrammerStruct *)d)->lasthaltmode)
	{
		return 0;
	}
	((struct LinkEProgrammerStruct *)d)->lasthaltmode = mode;

	if (mode == 0)
	{
		printf("Holding in reset\n");
		// Part one "immediately" places the part into reset.  Part 2 says when we're done, leave part in reset.
		wch_link_multicommands((libusb_device_handle *)dev, 2, 4, "\x81\x0d\x01\x02", 4, "\x81\x0d\x01\x01");
	}
	else if (mode == 1)
	{
		// This is clearly not the "best" method to exit reset.  I don't know why this combination works.
		wch_link_multicommands(
		    (libusb_device_handle *)dev, 3, 4, "\x81\x0b\x01\x01", 4, "\x81\x0d\x01\x02", 4, "\x81\x0d\x01\xff"
		);
	}
	else
	{
		return -999;
	}
	return 0;
}

#if 0
static int LEReadBinaryBlob( void * d, uint32_t offset, uint32_t amount, uint8_t * readbuff )
{
	libusb_device_handle * dev = ((struct LinkEProgrammerStruct*)d)->devh;

	InternalLinkEHaltMode( d, 0 );

	int i;
	int status;
	uint8_t rbuff[1024];
	int transferred = 0;
	int readbuffplace = 0;

	wch_link_command( (libusb_device_handle *)dev, "\x81\x06\x01\x01", 4, 0, 0, 0 );

	// Flush out any pending data.
	libusb_bulk_transfer( (libusb_device_handle *)dev, 0x82, rbuff, 1024, &transferred, 1 );

	// 3/8 = Read Memory
	// First 4 bytes are big-endian location.
	// Next 4 bytes are big-endian amount.
	uint8_t readop[11] = { 0x81, 0x03, 0x08, };

	readop[3] = (offset>>24)&0xff;
	readop[4] = (offset>>16)&0xff;
	readop[5] = (offset>>8)&0xff;
	readop[6] = (offset>>0)&0xff;

	readop[7] = (amount>>24)&0xff;
	readop[8] = (amount>>16)&0xff;
	readop[9] = (amount>>8)&0xff;
	readop[10] = (amount>>0)&0xff;

	wch_link_command( (libusb_device_handle *)dev, readop, 11, 0, 0, 0 );

	// Perform operation
	wch_link_command( (libusb_device_handle *)dev, "\x81\x02\x01\x0c", 4, 0, 0, 0 );

	uint32_t remain = amount;
	while( remain )
	{
		transferred = 0;
		WCHCHECK( libusb_bulk_transfer( (libusb_device_handle *)dev, 0x82, rbuff, 1024, &transferred, WCHTIMEOUT ) );
		memcpy( ((uint8_t*)readbuff) + readbuffplace, rbuff, transferred );
		readbuffplace += transferred;
		remain -= transferred;
	}

	// Flip internal endian.  Must be done separately in case something was unaligned when
	// reading.
	for( i = 0; i < readbuffplace/4; i++ )
	{
		uint32_t r = ((uint32_t*)readbuff)[i];
		((uint32_t*)readbuff)[i] = (r>>24) | ((r & 0xff0000) >> 8) | ((r & 0xff00)<<8) | (( r & 0xff )<<24);
	}

	return 0;
}
#endif

static int LEWriteBinaryBlob(void *d, uint32_t address_to_write, uint32_t len, uint8_t *blob)
{
	libusb_device_handle *dev = ((struct LinkEProgrammerStruct *)d)->devh;
	struct InternalState *iss = (struct InternalState *)(((struct LinkEProgrammerStruct *)d)->internal);

	InternalLinkEHaltMode(d, 0);

	int i;
	int status;
	uint8_t rbuff[1024];
	int transferred;

	int padlen = ((len - 1) & (~(iss->sector_size - 1))) + iss->sector_size;

	wch_link_command((libusb_device_handle *)dev, "\x81\x06\x01\x01", 4, 0, 0, 0);

	// Refs
	// https://github.com/cnlohr/ch32v003fun/issues/232
	// https://www.eevblog.com/forum/microcontrollers/ch32v307-risc-v-minicore-with-ethernet/msg5195862/#msg5195862
	switch (iss->wanted_ram_split)
	{
	// The byte marked with a v , is the Ram partition byte of interest

	case 32: // Below sets 32K Ram (0xff)                                v
		wch_link_command((libusb_device_handle *)dev, "\x81\x06\x08\x02\xff\xff\xff\xff\xff\xff\xff", 11, 0, 0, 0);
		break;
	case 64: // Below sets 64K Ram (0xbf)                                v
		wch_link_command((libusb_device_handle *)dev, "\x81\x06\x08\x02\xbf\xff\xff\xff\xff\xff\xff", 11, 0, 0, 0);
		break;
	case 96: // Below sets 96K Ram  (0x7f)                               v
		wch_link_command((libusb_device_handle *)dev, "\x81\x06\x08\x02\x7f\xff\xff\xff\xff\xff\xff", 11, 0, 0, 0);
		break;
	case 128: // Below sets 128K Ram (0x3f)                              v
		wch_link_command((libusb_device_handle *)dev, "\x81\x06\x08\x02\x3f\xff\xff\xff\xff\xff\xff", 11, 0, 0, 0);
		break;
	case 192: // Below sets 128K Ram (0x3f)                              v
		wch_link_command((libusb_device_handle *)dev, "\x81\x06\x08\x02\xDf\xff\xff\xff\xff\xff\xff", 11, 0, 0, 0);
		break;
	default: // Do nothing
		break;
	}

	wch_link_command(
	    (libusb_device_handle *)dev, "\x81\x06\x01\x01", 4, 0, 0, 0
	); // Not sure why but it seems to work better when we request twice.

	// This contains the write data quantity, in bytes.  (The last 2 octets)
	// Then it just rollllls on in.
	char rksbuff[11] =
	{
		0x81, 0x01, 0x08,
		// Address to write
		(uint8_t)(address_to_write >> 24), (uint8_t)(address_to_write >> 16), (uint8_t)(address_to_write >> 8),
		(uint8_t)(address_to_write & 0xff),
		// Length to write
		(uint8_t)(len >> 24), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)(len & 0xff)
	};
	wch_link_command((libusb_device_handle *)dev, rksbuff, 11, 0, 0, 0);

	wch_link_command((libusb_device_handle *)dev, "\x81\x02\x01\x05", 4, 0, 0, 0);

	const uint8_t *bootloader = GetFlashLoader(iss->target_chip_type);

	int pplace = 0;
	for (pplace = 0; pplace < bootloader_len; pplace += iss->sector_size)
	{
		WCHCHECK(libusb_bulk_transfer(
		             (libusb_device_handle *)dev, 0x02, (uint8_t *)(bootloader + pplace), iss->sector_size, &transferred,
		             WCHTIMEOUT
		         ));
	}

	for (i = 0; i < 10; i++)
	{
		wch_link_command((libusb_device_handle *)dev, "\x81\x02\x01\x07", 4, &transferred, rbuff, 1024);
		if (transferred == 4 && rbuff[0] == 0x82 && rbuff[1] == 0x02 && rbuff[2] == 0x01 && rbuff[3] == 0x07)
		{
			break;
		}
	}
	if (i == 10)
	{
		fprintf(stderr, "Error, confusing responses to 02/01/07\n");
		exit(-109);
	}

	wch_link_command((libusb_device_handle *)dev, "\x81\x02\x01\x02", 4, 0, 0, 0);

	for (pplace = 0; pplace < padlen; pplace += iss->sector_size)
	{
		if (pplace + iss->sector_size > len)
		{
			uint8_t paddeddata[iss->sector_size];
			int gap = pplace + iss->sector_size - len;
			int okcopy = len - pplace;
			memcpy(paddeddata, blob + pplace, okcopy);
			memset(paddeddata + okcopy, 0xff, gap);
			WCHCHECK(libusb_bulk_transfer(
			             (libusb_device_handle *)dev, 0x02, paddeddata, iss->sector_size, &transferred, WCHTIMEOUT
			         ));
		}
		else
		{
			WCHCHECK(libusb_bulk_transfer(
			             (libusb_device_handle *)dev, 0x02, blob + pplace, iss->sector_size, &transferred, WCHTIMEOUT
			         ));
		}
	}
	return 0;
}

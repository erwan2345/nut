#include "main.h"
#include "bcmxcp.h"
#include "bcmxcp_io.h"
#include "bool.h"
#include "common.h"
#include "nut_libusb.h"
#include "usb-common.h"
#include "timehead.h"
#include "nut_stdint.h" /* for uint16_t */
#include <ctype.h>
#include <sys/file.h>
#include <sys/types.h>
#include <unistd.h>

#define SUBDRIVER_NAME    "USB communication subdriver"
#define SUBDRIVER_VERSION "0.35"

/* communication driver description structure */
upsdrv_info_t bcmxcp_comm_upsdrv_info = {
	SUBDRIVER_NAME,
	SUBDRIVER_VERSION,
	NULL,
	0,
	{ NULL }
};

/** @brief Maximum number of attempts done when trying to (re-)open a device. */
#define MAX_TRY	5

/* Powerware */
#define POWERWARE 0x0592

/* Phoenixtec Power Co., Ltd */
#define PHOENIXTEC 0x06da

/* Hewlett Packard */
#define HP_VENDORID 0x03f0

/** @brief Currently used device. */
static USBDevice_t		 curDevice;

/* USB functions */
static bool_t	open_device(void);
/* unified failure reporting: call these often */
void nutusb_comm_fail(const char *fmt, ...)
	__attribute__ ((__format__ (__printf__, 1, 2)));
void nutusb_comm_good(void);
/* function pointer, set depending on which device is used */
int (*usb_set_descriptor)(libusb_device_handle *udev, unsigned char type,
	unsigned char index, void *buf, int size);



/* usb_set_descriptor() for Powerware devices */
static int usb_set_powerware(libusb_device_handle *udev, unsigned char type, unsigned char index, void *buf, int size)
{
	return libusb_control_transfer(udev, LIBUSB_ENDPOINT_OUT, LIBUSB_REQUEST_SET_DESCRIPTOR, (type << 8) | index, 0, buf, size, 1000);
}

static void *powerware_ups(USBDevice_t *device) {
	usb_set_descriptor = &usb_set_powerware;
	return NULL;
}

/* usb_set_descriptor() for Phoenixtec devices */
static int usb_set_phoenixtec(libusb_device_handle *udev, unsigned char type, unsigned char index, void *buf, int size)
{
	return libusb_control_transfer(udev, 0x42, 0x0d, (0x00 << 8) | 0x0, 0, buf, size, 1000);
}

static void *phoenixtec_ups(USBDevice_t *device) {
	usb_set_descriptor = &usb_set_phoenixtec;
	return NULL;
}

/* USB IDs device table */
static usb_device_id_t pw_usb_device_table[] = {
	/* various models */
	{ USB_DEVICE(POWERWARE, 0x0002), &powerware_ups },

	/* various models */
	{ USB_DEVICE(PHOENIXTEC, 0x0002), &phoenixtec_ups },

	/* T500 */
	{ USB_DEVICE(HP_VENDORID, 0x1f01), &phoenixtec_ups },
	/* T750 */
	{ USB_DEVICE(HP_VENDORID, 0x1f02), &phoenixtec_ups },
	
	/* Terminating entry */
	{ -1, -1, NULL }
};

/** @brief Actual matching function for @ref device_matcher.
 *
 * By calling is_usb_device_supported() on @ref pw_usb_device_table, this also sets @ref usb_set_descriptor as per *device*. */
static int	device_match_func(USBDevice_t *device, void *privdata)
{
	switch (is_usb_device_supported(pw_usb_device_table, device))
	{
	case SUPPORTED:
		return 1;
	case POSSIBLY_SUPPORTED:
	case NOT_SUPPORTED:
	default:
		return 0;
	}
}

/** @brief Matcher to test available devices.
 *
 * This is used as the starting point for (re-)matching a device when initialising (alongside @ref regex_matcher) or when reconnecting (alongside @ref regex_matcher and @ref exact_matcher). */
static USBDeviceMatcher_t	device_matcher = {
	&device_match_func,
	NULL,
	NULL
};

/** @brief Regex matcher compiled starting from user-provided values that has to be matched by a USB device to be accepted. */
static USBDeviceMatcher_t	*regex_matcher = NULL;

/** @brief Exact matcher compiled starting from the discovered device for later reopening it. */
static USBDeviceMatcher_t	*exact_matcher = NULL;

/* limit the amount of spew that goes in the syslog when we lose the UPS */
#define USB_ERR_LIMIT 10 /* start limiting after 10 in a row  */
#define USB_ERR_RATE 10  /* then only print every 10th error */
#define XCP_USB_TIMEOUT 5000

/* global variables */
libusb_device_handle *upsdev = NULL;
extern int exit_flag;
static unsigned int comm_failures = 0;

/* Functions implementations */
void send_read_command(unsigned char command)
{
	unsigned char buf[4];

	if (upsdev) {
		buf[0] = PW_COMMAND_START_BYTE;
		buf[1] = 0x01;                    /* data length */
		buf[2] = command;                 /* command to send */
		buf[3] = calc_checksum(buf);      /* checksum */
		upsdebug_hex (3, "send_read_command", buf, 4);
		usb_set_descriptor(upsdev, LIBUSB_DT_STRING, 4, buf, 4); /* FIXME: Ignore error */
	}
}

void send_write_command(unsigned char *command, int command_length)
{
	unsigned char sbuf[128];

	if (upsdev) {
		/* Prepare the send buffer */
		sbuf[0] = PW_COMMAND_START_BYTE;
		sbuf[1] = (unsigned char)(command_length);
		memcpy(sbuf+2, command, command_length);
		command_length += 2;

		/* Add checksum */
		sbuf[command_length] = calc_checksum(sbuf);
		command_length += 1;
		upsdebug_hex (3, "send_write_command", sbuf, command_length);
		usb_set_descriptor(upsdev, LIBUSB_DT_STRING, 4, sbuf, command_length);  /* FIXME: Ignore error */
	}
}

#define PW_HEADER_SIZE (PW_HEADER_LENGTH + 1)
#define PW_CMD_BUFSIZE	256
/* get the answer of a command from the ups. And check that the answer is for this command */
int get_answer(unsigned char *data, unsigned char command)
{
	unsigned char buf[PW_CMD_BUFSIZE], *my_buf = buf;
	int length, end_length, ret, endblock, bytes_read, ellapsed_time, need_data;
	int tail, transferred;
	unsigned char block_number, sequence, seq_num;
	struct timeval start_time, now;

	if (upsdev == NULL)
		return -1;

	need_data = PW_HEADER_SIZE; /* 4 - cmd response header length, 1 for csum */
	end_length = 0;    /* total length of sequence(s), not counting header(s) */
	endblock = 0;      /* signal the last sequence in the block */
	bytes_read = 0;    /* total length of data read, including XCP header */
	transferred = 0;
	ellapsed_time = 0;
	seq_num = 1;       /* current theoric sequence */

	upsdebugx(1, "entering get_answer(%x)", command);

	/* Store current time */
	gettimeofday(&start_time, NULL);
	memset(&buf, 0x0, PW_CMD_BUFSIZE);

	while ( (!endblock) && ((XCP_USB_TIMEOUT - ellapsed_time)  > 0) ) {

		/* Get (more) data if needed */
		if (need_data > 0) {

			ret = libusb_interrupt_transfer(upsdev, LIBUSB_ENDPOINT_IN | 1, buf + bytes_read, 128, &transferred, XCP_USB_TIMEOUT - ellapsed_time);

			/* Update time */
			gettimeofday(&now, NULL);
			ellapsed_time = (now.tv_sec - start_time.tv_sec)*1000 +
					(now.tv_usec - start_time.tv_usec)/1000;

			/* Check libusb return value */
			if (ret != LIBUSB_SUCCESS) {
				/* Clear any possible endpoint stalls */
				libusb_clear_halt(upsdev, LIBUSB_ENDPOINT_IN | 1);
				/* continue; */ /* FIXME: seems a break would be better! */
				break;
			}

			/* this seems to occur on XSlot USB card */
			if (transferred == 0) {
				/* FIXME: */
				continue;
			}
			/* Else, we got some input bytes */
			bytes_read += transferred;
			need_data -= transferred;
			upsdebug_hex(1, "get_answer", buf, bytes_read);
		}

		if (need_data > 0) /* We need more data */
		    continue;

		/* Now validate XCP frame */
		/* Check header */
		if ( my_buf[0] != PW_COMMAND_START_BYTE ) {
			upsdebugx(2, "get_answer: wrong header 0xab vs %02x", my_buf[0]);
			/* Sometime we read something wrong. bad cables? bad ports? */
			my_buf = memchr(my_buf, PW_COMMAND_START_BYTE, bytes_read);
			if (!my_buf)
			    return -1;
		}

		/* Read block number byte */
		block_number = my_buf[1];
		upsdebugx(1, "get_answer: block_number = %x", block_number);

		/* Check data length byte (remove the header length) */
		length = my_buf[2];
		upsdebugx(3, "get_answer: data length = %d", length);
		if (bytes_read - (length + PW_HEADER_SIZE) < 0) {
			if (need_data < 0) --need_data; /* count zerro byte too */
			need_data += length + 1; /* packet lenght + checksum */
			upsdebugx(2, "get_answer: need to read %d more data", need_data);
			continue;
		}
		/* Check if Length conforms to XCP (121 for normal, 140 for Test mode) */
		/* Use the more generous length for testing */
		if (length > 140) {
			upsdebugx(2, "get_answer: bad length");
			return -1;
		}

		/* Test the Sequence # */
		sequence = my_buf[3];
		if ((sequence & PW_SEQ_MASK) != seq_num) {
			nutusb_comm_fail("get_answer: not the right sequence received %x!!!\n", (sequence & PW_SEQ_MASK));
			return -1;
		}
		else {
			upsdebugx(2, "get_answer: sequence number (%x) is ok", (sequence & PW_SEQ_MASK));
		}

		/* Validate checksum */
		if (!checksum_test(my_buf)) {
			nutusb_comm_fail("get_answer: checksum error! ");
			return -1;
		}
		else {
			upsdebugx(2, "get_answer: checksum is ok");
		}

		/* Check if it's the last sequence */
		if (sequence & PW_LAST_SEQ) {
			/* we're done receiving data */
			upsdebugx(2, "get_answer: all data received");
			endblock = 1;
		}
		else {
			seq_num++;
			upsdebugx(2, "get_answer: next sequence is %d", seq_num);
		}

		/* copy the current valid XCP frame back */
		memcpy(data+end_length, my_buf + 4, length);
		/* increment pointers to process the next sequence */
		end_length += length;
		tail = bytes_read - (length + PW_HEADER_SIZE);
		if (tail > 0)
		    my_buf = memmove(&buf[0], my_buf + length + PW_HEADER_SIZE, tail);
		else if (tail == 0)
		    my_buf = &buf[0];
		bytes_read = tail;
	}

	upsdebug_hex (5, "get_answer", data, end_length);
	return end_length;
}

/* Sends a single command (length=1). and get the answer */
int command_read_sequence(unsigned char command, unsigned char *data)
{
	int bytes_read = 0;
	int retry = 0;
	
	while ((bytes_read < 1) && (retry < 5)) {
		send_read_command(command);
		bytes_read = get_answer(data, command);
		retry++;
	}

	if (bytes_read < 1) {
		nutusb_comm_fail("Error executing command");
		dstate_datastale();
		return -1;
	}

	return bytes_read;
}

/* Sends a setup command (length > 1) */
int command_write_sequence(unsigned char *command, int command_length, unsigned char *answer)
{
	int bytes_read = 0;
	int retry = 0;

	while ((bytes_read < 1) && (retry < 5)) {
		send_write_command(command, command_length);
		sleep(PW_SLEEP);
		bytes_read = get_answer(answer, command[0]);
		retry ++;
	}

	if (bytes_read < 1) {
		nutusb_comm_fail("Error executing command");
		dstate_datastale();
		return -1;
	}

	return bytes_read;
}


void upsdrv_comm_good(void)
{
	nutusb_comm_good();
}

void	upsdrv_initups(void)
{
	int	 ret;
	char	*regex_array[6];

	upsdebugx(1, "%s()", __func__);

	/* Get user-provided values... */
	regex_array[0] = getval("vendorid");
	regex_array[1] = getval("productid");
	regex_array[2] = getval("vendor");
	regex_array[3] = getval("product");
	regex_array[4] = getval("serial");
	regex_array[5] = getval("bus");

	/* ...and create a regex matcher from them */
	ret = USBNewRegexMatcher(&regex_matcher, regex_array, REG_ICASE | REG_EXTENDED);
	switch (ret)
	{
	case  0:
		break;	/* All is well */
	case -1:
		fatal_with_errno(EXIT_FAILURE, "USBNewRegexMatcher");
	default:
		fatalx(EXIT_FAILURE, "Invalid regular expression: %s", regex_array[ret]);
	}

	/* Link the matchers */
	device_matcher.next = regex_matcher;

	/* Initialise the communication subdriver */
	usb_subdriver.init();

	/* Try to open the device */
	if (!open_device())
		fatalx(
			EXIT_FAILURE,
			"Unable to find a USB POWERWARE device.\n\n"

			"Things to try:\n"
			" - Connect the device to a USB bus\n"
			" - Run this driver as another user (upsdrvctl -u or 'user=...' in ups.conf).\n"
			"   See upsdrvctl(8) and ups.conf(5).\n\n"

			"Fatal error: unusable configuration."
		);

	/* Create a new exact matcher for later reopening */
	ret = USBNewExactMatcher(&exact_matcher, &curDevice);
	if (ret)
		fatal_with_errno(EXIT_FAILURE, "USBNewExactMatcher");

	/* Link the matchers */
	regex_matcher->next = exact_matcher;

	dstate_setinfo("ups.vendorid", "%04x", curDevice.VendorID);
	dstate_setinfo("ups.productid", "%04x", curDevice.ProductID);
}

/** @brief Try to open a USB device matching @ref device_matcher.
 *
 * If @ref upsdev refers to an already opened device (i.e. it is not `NULL`), it is closed before attempting the reopening.
 *
 * @return @ref TRUE, with @ref upsdev being the handle of the opened device, on success,
 * @return @ref FALSE, with @ref upsdev being `NULL`, on failure. */
static bool_t	open_device(void)
{
	size_t	try;

	for (try = 1; try <= MAX_TRY; try++) {
		int	ret;

		ret = usb_subdriver.open(&upsdev, &curDevice, &device_matcher, NULL);
		if (ret != LIBUSB_SUCCESS)
			continue;

		ret = libusb_clear_halt(upsdev, LIBUSB_ENDPOINT_IN | 1);
		if (ret != LIBUSB_SUCCESS) {
			upsdebugx(1, "%s: can't reset POWERWARE USB endpoint: %s.", __func__, libusb_strerror(ret));
			libusb_reset_device(upsdev);
			usb_subdriver.close(upsdev);
			upsdev = NULL;
			/* Wait reconnect */
			sleep(5);
			continue;
		}

		return TRUE;
	}

	return FALSE;
}

void	upsdrv_cleanup(void)
{
	upsdebugx(1, "%s()", __func__);

	usb_subdriver.close(upsdev);
	usb_subdriver.deinit();

	USBFreeExactMatcher(exact_matcher);
	USBFreeRegexMatcher(regex_matcher);

	free(curDevice.Vendor);
	free(curDevice.Product);
	free(curDevice.Serial);
	free(curDevice.Bus);
}

void	upsdrv_reconnect(void)
{
	upsdebugx(4, "==================================================");
	upsdebugx(4, "= device has been disconnected, try to reconnect =");
	upsdebugx(4, "==================================================");

	if (open_device())
		upsdebugx(4, "%s: successfully reconnected to device %04x:%04x.", __func__, curDevice.VendorID, curDevice.ProductID);
	else
		upsdebugx(4, "%s: cannot reconnect to device %04x:%04x.", __func__, curDevice.VendorID, curDevice.ProductID);
}

void	bcmxcp_comm_upsdrv_makevartable(void)
{
	usb_subdriver.add_nutvars();
}

void nutusb_comm_fail(const char *fmt, ...)
{
	int ret;
	char why[SMALLBUF];
	va_list ap;

	/* this means we're probably here because select was interrupted */
	if (exit_flag != 0)
		return; /* ignored, since we're about to exit anyway */

	comm_failures++;

	if ((comm_failures == USB_ERR_LIMIT) ||
		((comm_failures % USB_ERR_RATE) == 0))
	{
		upslogx(LOG_WARNING, "Warning: excessive comm failures, "
			"limiting error reporting");
	}

	/* once it's past the limit, only log once every USB_ERR_LIMIT calls */
	if ((comm_failures > USB_ERR_LIMIT) &&
		((comm_failures % USB_ERR_LIMIT) != 0)) {
		/* Try reconnection */
		upsdebugx(1, "Got to reconnect!\n");
		upsdrv_reconnect();
		return;
	}

	/* generic message if the caller hasn't elaborated */
	if (!fmt)
	{
		upslogx(LOG_WARNING, "Communications with UPS lost - check cabling");
		return;
	}

	va_start(ap, fmt);
	ret = vsnprintf(why, sizeof(why), fmt, ap);
	va_end(ap);

	if ((ret < 1) || (ret >= (int) sizeof(why)))
		upslogx(LOG_WARNING, "usb_comm_fail: vsnprintf needed "
			"more than %d bytes", (int)sizeof(why));

	upslogx(LOG_WARNING, "Communications with UPS lost: %s", why);
}

void nutusb_comm_good(void)
{
	if (comm_failures == 0)
		return;

	upslogx(LOG_NOTICE, "Communications with UPS re-established");
	comm_failures = 0;
}


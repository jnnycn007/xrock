#include <rock.h>

static struct chip_t chips[] = {
	{ 0x110c, "RK1106" },
	{ 0x180a, "RK1808" },
	{ 0x281a, "RK2818" },
	{ 0x290a, "RK2918" },
	{ 0x292a, "RK2928" },
	{ 0x292c, "RK3026" },
	{ 0x300a, "RK3066" },
	{ 0x300b, "RK3168" },
	{ 0x301a, "RK3036" },
	{ 0x310a, "RK3066" },
	{ 0x310b, "RK3188" },
	{ 0x310c, "RK3128" },
	{ 0x320a, "RK3288" },
	{ 0x320b, "RK3228" },
	{ 0x320c, "RK3328" },
	{ 0x330a, "RK3368" },
	{ 0x330c, "RK3399" },
	{ 0x330d, "PX30" },
	{ 0x330e, "RK3308" },
	{ 0x350a, "RK3568" },
	{ 0x350b, "RK3588" },
	{ 0x350d, "RK3562" },
	{ 0x350e, "RK3576" },
};

static struct chip_t chip_unknown = {
	0x0000, "UNKNOWN"
};

int xrock_init(struct xrock_ctx_t * ctx)
{
	if(ctx)
	{
		libusb_device ** list = NULL;
		int found = 0;

		ctx->hdl = NULL;
		ctx->chip = NULL;
		for(int count = 0; (count < libusb_get_device_list(ctx->context, &list)) && !found; count++)
		{
			struct libusb_device_descriptor desc;
			libusb_device * device = list[count];
			libusb_device_handle * hdl;
			if(libusb_get_device_descriptor(device, &desc) == 0)
			{
				if(desc.idVendor == 0x2207)
				{
					for(int i = 0; i < ARRAY_SIZE(chips); i++)
					{
						if(desc.idProduct == chips[i].pid)
						{
							if(libusb_open(device, &hdl) == 0)
							{
								ctx->hdl = hdl;
								ctx->chip = &chips[i];
								found = 1;
								break;
							}
						}
					}
					if(!found)
					{
						if(libusb_open(device, &hdl) == 0)
						{
							ctx->hdl = hdl;
							ctx->chip = &chip_unknown;
							found = 1;
							break;
						}
					}
				}
			}
		}

		if(ctx->hdl && ctx->chip && found)
		{
			if(libusb_kernel_driver_active(ctx->hdl, 0))
				libusb_detach_kernel_driver(ctx->hdl, 0);

			if(libusb_claim_interface(ctx->hdl, 0) == 0)
			{
				struct libusb_device_descriptor desc;
				if(libusb_get_device_descriptor(libusb_get_device(ctx->hdl), &desc) == 0)
				{
					if((desc.bcdUSB & 0x0001) == 0x0000)
						ctx->maskrom = 1;
					else
						ctx->maskrom = 0;

					struct libusb_config_descriptor * config;
					if(libusb_get_active_config_descriptor(libusb_get_device(ctx->hdl), &config) == 0)
					{
						for(int if_idx = 0; if_idx < config->bNumInterfaces; if_idx++)
						{
							const struct libusb_interface * iface = config->interface + if_idx;
							for(int set_idx = 0; set_idx < iface->num_altsetting; set_idx++)
							{
								const struct libusb_interface_descriptor * setting = iface->altsetting + set_idx;
								for(int ep_idx = 0; ep_idx < setting->bNumEndpoints; ep_idx++)
								{
									const struct libusb_endpoint_descriptor * ep = setting->endpoint + ep_idx;
									if((ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) != LIBUSB_TRANSFER_TYPE_BULK)
										continue;
									if((ep->bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN)
										ctx->epin = ep->bEndpointAddress;
									else
										ctx->epout = ep->bEndpointAddress;
								}
							}
						}
						libusb_free_config_descriptor(config);
						return 1;
					}
		    	}
		    }
		}
	}
	return 0;
}

void rock_maskrom_upload_memory(struct xrock_ctx_t * ctx, uint32_t code, void * buf, uint64_t len, int rc4)
{
	struct rc4_ctx_t rctx;
	uint8_t key[16] = { 124, 78, 3, 4, 85, 5, 9, 7, 45, 44, 123, 56, 23, 13, 23, 17 };
	uint64_t total = 0;
	uint16_t crc16 = 0xffff;
	int pend = 0;
	unsigned char * buffer;

	buffer = malloc(len + 5);
	if(buffer)
	{
		memset(buffer, 0, len + 5);
		memcpy(buffer, buf, len);
		if(rc4)
		{
			rc4_setkey(&rctx, key, sizeof(key));
			rc4_crypt(&rctx, buffer, len);
		}
		switch(len % 4096)
		{
		case 4095:
			len++;
			break;
		case 4094:
			pend = 1;
			break;
		case 0:
		default:
			break;
		}

		crc16 = crc16_sum(crc16, buffer, len);
		buffer[len++] = crc16 >> 8;
		buffer[len++] = crc16 & 0xff;
		while(total < len)
		{
			int n = ((len - total) > 4096) ? 4096 : (len - total);
			if(libusb_control_transfer(ctx->hdl, LIBUSB_REQUEST_TYPE_VENDOR, 0xc, 0, code, buffer + total, n, 0) != n)
			{
				free(buffer);
				return;
			}
			total += n;
		}
		if(pend)
		{
			unsigned char zero = 0;
			libusb_control_transfer(ctx->hdl, LIBUSB_REQUEST_TYPE_VENDOR, 0xc, 0, code, &zero, 1, 0);
		}
		free(buffer);
	}
}

void rock_maskrom_upload_file(struct xrock_ctx_t * ctx, uint32_t code, const char * filename, int rc4)
{
	uint64_t len;
	void * buf;

	buf = file_load(filename, &len);
	if(buf)
	{
		rock_maskrom_upload_memory(ctx, code, buf, len, rc4);
		free(buf);
	}
}

enum {
	USB_REQUEST_SIGN		= 0x55534243,	/* "USBC" */
	USB_RESPONSE_SIGN		= 0x55534253,	/* "USBS" */
};

enum {
	USB_DIRECTION_OUT		= 0x00,
	USB_DIRECTION_IN		= 0x80,
};

enum {
	OPCODE_TEST_UNIT_READY	= 0x00,
	OPCODE_READ_FLASH_ID	= 0x01,
	OPCODE_TEST_BAD_BLOCK	= 0x03,
	OPCODE_READ_SECTOR		= 0x04,
	OPCODE_WRITE_SECTOR		= 0x05,
	OPCODE_ERASE_NORMAL		= 0x06,
	OPCODE_ERASE_FORCE		= 0x0b,
	OPCODE_READ_LBA			= 0x14,
	OPCODE_WRITE_LBA		= 0x15,
	OPCODE_ERASE_SYSTEM		= 0x16,
	OPCODE_READ_SDRAM		= 0x17,
	OPCODE_WRITE_SDRAM		= 0x18,
	OPCODE_EXEC_SDRAM		= 0x19,
	OPCODE_READ_FLASH_INFO	= 0x1a,
	OPCODE_READ_CHIP_INFO	= 0x1b,
	OPCODE_SET_RESET_FLAG	= 0x1e,
	OPCODE_WRITE_EFUSE		= 0x1f,
	OPCODE_READ_EFUSE		= 0x20,
	OPCODE_READ_SPI_FLASH	= 0x21,
	OPCODE_WRITE_SPI_FLASH	= 0x22,
	OPCODE_WRITE_NEW_EFUSE	= 0x23,
	OPCODE_READ_NEW_EFUSE	= 0x24,
	OPCODE_ERASE_LBA		= 0x25,
	OPCODE_READ_COM_LOG		= 0x28,
	OPCODE_SWITCH_STORAGE	= 0x2a,
	OPCODE_READ_STORAGE		= 0x2b,
	OPCODE_READ_OTP_CHIP	= 0x2c,
	OPCODE_READ_CAPABILITY	= 0xaa,
	OPCODE_SWITCH_USB3		= 0xbb,
	OPCODE_RESET_DEVICE		= 0xff,
};

struct usb_command_t {
	uint8_t opcode;				/* Opcode */
	uint8_t subcode;			/* Subcode */
	uint8_t address[4];			/* Address */
	uint8_t reserved6;
	uint8_t size[2];			/* Size */
	uint8_t reserved9;
	uint8_t reserved10;
	uint8_t reserved11;
	uint8_t reserved12[4];
} __attribute__((packed));

struct usb_request_t {
	uint8_t signature[4];		/* Contains 'USBC' */
	uint8_t tag[4];				/* The random unique id */
	uint8_t length[4];			/* The data transfer length */
	uint8_t flag;				/* Direction in bit 7, IN(0x80), OUT(0x00) */
	uint8_t lun;				/* Lun (Flash chip select, normally 0) */
	uint8_t cmdlen;				/* Command Length 6/10/16 */
	struct usb_command_t cmd;
} __attribute__((packed));

struct usb_response_t {
	uint8_t signature[4];		/* Contains 'USBS' */
	uint8_t tag[4];				/* Same as original command */
	uint8_t residue[4];			/* Amount not transferred */
	uint8_t status;				/* Response status */
} __attribute__((packed));

static inline void usb_bulk_send(libusb_device_handle * hdl, int ep, void * buf, size_t len)
{
	size_t max_chunk = 128 * 1024;
	size_t chunk;
	int r, bytes;

	while(len > 0)
	{
		chunk = len < max_chunk ? len : max_chunk;
		r = libusb_bulk_transfer(hdl, ep, (void *)buf, chunk, &bytes, 2000);
		if(r != 0)
		{
			printf("usb bulk send error\r\n");
			exit(-1);
		}
		len -= bytes;
		buf += bytes;
	}
}

static inline void usb_bulk_recv(libusb_device_handle * hdl, int ep, void * buf, size_t len)
{
	int r, bytes;

	while(len > 0)
	{
		r = libusb_bulk_transfer(hdl, ep, (void *)buf, len, &bytes, 2000);
		if(r != 0)
		{
			printf("usb bulk recv error\r\n");
			exit(-1);
		}
		len -= bytes;
		buf += bytes;
	}
}

static inline uint32_t make_tag(void)
{
	uint32_t tag = 0;
	int i;

	for(i = 0; i < 4; i++)
		tag = (tag << 8) | (rand() & 0xff);
	return tag;
}

int rock_ready(struct xrock_ctx_t * ctx)
{
	struct usb_request_t req;
	struct usb_response_t res;

	memset(&req, 0, sizeof(struct usb_request_t));
	write_be32(&req.signature[0], USB_REQUEST_SIGN);
	write_be32(&req.tag[0], make_tag());
	write_be32(&req.length[0], 0);
	req.flag = USB_DIRECTION_IN;
	req.cmdlen = 6;
	req.cmd.opcode = OPCODE_TEST_UNIT_READY;
	req.cmd.subcode = 0;

	usb_bulk_send(ctx->hdl, ctx->epout, &req, sizeof(struct usb_request_t));
	usb_bulk_recv(ctx->hdl, ctx->epin, &res, sizeof(struct usb_response_t));
	if((read_be32(&res.signature[0]) != USB_RESPONSE_SIGN) || (memcmp(&res.tag[0], &req.tag[0], 4) != 0))
		return 0;
	return 1;
}

int rock_version(struct xrock_ctx_t * ctx, uint8_t * buf)
{
	struct usb_request_t req;
	struct usb_response_t res;

	memset(&req, 0, sizeof(struct usb_request_t));
	write_be32(&req.signature[0], USB_REQUEST_SIGN);
	write_be32(&req.tag[0], make_tag());
	write_be32(&req.length[0], 16);
	req.flag = USB_DIRECTION_IN;
	req.cmdlen = 6;
	req.cmd.opcode = OPCODE_READ_CHIP_INFO;

	usb_bulk_send(ctx->hdl, ctx->epout, &req, sizeof(struct usb_request_t));
	usb_bulk_recv(ctx->hdl, ctx->epin, buf, 16);
	usb_bulk_recv(ctx->hdl, ctx->epin, &res, sizeof(struct usb_response_t));
	if((read_be32(&res.signature[0]) != USB_RESPONSE_SIGN) || (memcmp(&res.tag[0], &req.tag[0], 4) != 0))
		return 0;
	return 1;
}

int rock_capability(struct xrock_ctx_t * ctx, uint8_t * buf)
{
	struct usb_request_t req;
	struct usb_response_t res;

	memset(&req, 0, sizeof(struct usb_request_t));
	write_be32(&req.signature[0], USB_REQUEST_SIGN);
	write_be32(&req.tag[0], make_tag());
	write_be32(&req.length[0], 8);
	req.flag = USB_DIRECTION_IN;
	req.cmdlen = 6;
	req.cmd.opcode = OPCODE_READ_CAPABILITY;
	usb_bulk_send(ctx->hdl, ctx->epout, &req, sizeof(struct usb_request_t));
	usb_bulk_recv(ctx->hdl, ctx->epin, buf, 8);
	usb_bulk_recv(ctx->hdl, ctx->epin, &res, sizeof(struct usb_response_t));
	if((read_be32(&res.signature[0]) != USB_RESPONSE_SIGN) || (memcmp(&res.tag[0], &req.tag[0], 4) != 0))
		return 0;
	return 1;
}

int rock_reset(struct xrock_ctx_t * ctx, int maskrom)
{
	struct usb_request_t req;
	struct usb_response_t res;

	memset(&req, 0, sizeof(struct usb_request_t));
	write_be32(&req.signature[0], USB_REQUEST_SIGN);
	write_be32(&req.tag[0], make_tag());
	write_be32(&req.length[0], 0);
	req.flag = USB_DIRECTION_OUT;
	req.cmdlen = 6;
	req.cmd.opcode = OPCODE_RESET_DEVICE;
	req.cmd.subcode = maskrom ? 0x03 : 0x00;

	usb_bulk_send(ctx->hdl, ctx->epout, &req, sizeof(struct usb_request_t));
	usb_bulk_recv(ctx->hdl, ctx->epin, &res, sizeof(struct usb_response_t));
	if((read_be32(&res.signature[0]) != USB_RESPONSE_SIGN) || (memcmp(&res.tag[0], &req.tag[0], 4) != 0))
		return 0;
	return 1;
}

int rock_exec(struct xrock_ctx_t * ctx, uint32_t addr, uint32_t dtb)
{
	struct usb_request_t req;
	struct usb_response_t res;

	memset(&req, 0, sizeof(struct usb_request_t));
	write_be32(&req.signature[0], USB_REQUEST_SIGN);
	write_be32(&req.tag[0], make_tag());
	write_be32(&req.length[0], 0);
	req.flag = USB_DIRECTION_OUT;
	req.cmdlen = 10;
	req.cmd.opcode = OPCODE_EXEC_SDRAM;
	req.cmd.subcode = 0xaa;
	write_be32(&req.cmd.address[0], (uint32_t)addr);
	write_be32(&req.cmd.size[0], (uint32_t)dtb);

	usb_bulk_send(ctx->hdl, ctx->epout, &req, sizeof(struct usb_request_t));
	usb_bulk_recv(ctx->hdl, ctx->epin, &res, sizeof(struct usb_response_t));
	if((read_be32(&res.signature[0]) != USB_RESPONSE_SIGN) || (memcmp(&res.tag[0], &req.tag[0], 4) != 0))
		return 0;
	return 1;
}

static inline int rock_read_raw(struct xrock_ctx_t * ctx, uint32_t addr, void * buf, size_t len)
{
	struct usb_request_t req;
	struct usb_response_t res;

	memset(&req, 0, sizeof(struct usb_request_t));
	write_be32(&req.signature[0], USB_REQUEST_SIGN);
	write_be32(&req.tag[0], make_tag());
	write_be32(&req.length[0], 0);
	req.flag = USB_DIRECTION_IN;
	req.cmdlen = 10;
	req.cmd.opcode = OPCODE_READ_SDRAM;
	write_be32(&req.cmd.address[0], addr);
	write_be16(&req.cmd.size[0], (uint16_t)len);

	usb_bulk_send(ctx->hdl, ctx->epout, &req, sizeof(struct usb_request_t));
	usb_bulk_recv(ctx->hdl, ctx->epin, buf, len);
	usb_bulk_recv(ctx->hdl, ctx->epin, &res, sizeof(struct usb_response_t));
	if((read_be32(&res.signature[0]) != USB_RESPONSE_SIGN) || (memcmp(&res.tag[0], &req.tag[0], 4) != 0))
		return 0;
	return 1;
}

static inline int rock_write_raw(struct xrock_ctx_t * ctx, uint32_t addr, void * buf, size_t len)
{
	struct usb_request_t req;
	struct usb_response_t res;

	memset(&req, 0, sizeof(struct usb_request_t));
	write_be32(&req.signature[0], USB_REQUEST_SIGN);
	write_be32(&req.tag[0], make_tag());
	write_be32(&req.length[0], 0);
	req.flag = USB_DIRECTION_OUT;
	req.cmdlen = 10;
	req.cmd.opcode = OPCODE_WRITE_SDRAM;
	write_be32(&req.cmd.address[0], addr);
	write_be16(&req.cmd.size[0], (uint16_t)len);

	usb_bulk_send(ctx->hdl, ctx->epout, &req, sizeof(struct usb_request_t));
	usb_bulk_send(ctx->hdl, ctx->epout, buf, len);
	usb_bulk_recv(ctx->hdl, ctx->epin, &res, sizeof(struct usb_response_t));
	if((read_be32(&res.signature[0]) != USB_RESPONSE_SIGN) || (memcmp(&res.tag[0], &req.tag[0], 4) != 0))
		return 0;
	return 1;
}

int rock_read(struct xrock_ctx_t * ctx, uint32_t addr, void * buf, size_t len)
{
	size_t n;

	while(len > 0)
	{
		n = len > 16384 ? 16384 : len;
		if(!rock_read_raw(ctx, addr, buf, n))
			return 0;
		addr += n;
		buf += n;
		len -= n;
	}
	return 1;
}

int rock_write(struct xrock_ctx_t * ctx, uint32_t addr, void * buf, size_t len)
{
	size_t n;

	while(len > 0)
	{
		n = len > 16384 ? 16384 : len;
		if(!rock_write_raw(ctx, addr, buf, n))
			return 0;
		addr += n;
		buf += n;
		len -= n;
	}
	return 1;
}

int rock_read_progress(struct xrock_ctx_t * ctx, uint32_t addr, void * buf, size_t len)
{
	struct progress_t p;
	size_t n;

	progress_start(&p, len);
	while(len > 0)
	{
		n = len > 16384 ? 16384 : len;
		if(!rock_read_raw(ctx, addr, buf, n))
			return 0;
		addr += n;
		buf += n;
		len -= n;
		progress_update(&p, n);
	}
	progress_stop(&p);
	return 1;
}

int rock_write_progress(struct xrock_ctx_t * ctx, uint32_t addr, void * buf, size_t len)
{
	struct progress_t p;
	size_t n;

	progress_start(&p, len);
	while(len > 0)
	{
		n = len > 16384 ? 16384 : len;
		if(!rock_write_raw(ctx, addr, buf, n))
			return 0;
		addr += n;
		buf += n;
		len -= n;
		progress_update(&p, n);
	}
	progress_stop(&p);
	return 1;
}

int rock_sn_read(struct xrock_ctx_t * ctx, char * sn)
{
	if(sn)
	{
		char buf[512];
		if(rock_flash_read_lba(ctx, 0xfff00001, 1, buf))
		{
			uint32_t valid = read_le32(&buf[0]);
			uint32_t len = read_le32(&buf[4]);
			if((valid == 1) && (len >= 0) && (len <= 512 - 8))
			{
				memcpy(sn, &buf[8], len);
				sn[len] = '\0';
				return 1;
			}
		}
	}
	return 0;
}

int rock_sn_write(struct xrock_ctx_t * ctx, char * sn)
{
	if(sn)
	{
		int len = strlen(sn);
		if((len >= 0) && (len <= 512 - 8))
		{
			char buf[512];
			memset(buf, 0, sizeof(buf));
			write_le32(&buf[0], 1);
			write_le32(&buf[4], len);
			memcpy(&buf[8], sn, len);
			return rock_flash_write_lba(ctx, 0xfff00001, 1, buf);
		}
	}
	return 0;
}

int rock_flash_detect(struct xrock_ctx_t * ctx, struct flash_info_t * info)
{
	struct usb_request_t req;
	struct usb_response_t res;

	memset(&req, 0, sizeof(struct usb_request_t));
	write_be32(&req.signature[0], USB_REQUEST_SIGN);
	write_be32(&req.tag[0], make_tag());
	write_be32(&req.length[0], 11);
	req.flag = USB_DIRECTION_IN;
	req.cmdlen = 6;
	req.cmd.opcode = OPCODE_READ_FLASH_INFO;
	usb_bulk_send(ctx->hdl, ctx->epout, &req, sizeof(struct usb_request_t));
	usb_bulk_recv(ctx->hdl, ctx->epin, info, 11);
	usb_bulk_recv(ctx->hdl, ctx->epin, &res, sizeof(struct usb_response_t));
	if((read_be32(&res.signature[0]) != USB_RESPONSE_SIGN) || (memcmp(&res.tag[0], &req.tag[0], 4) != 0))
		return 0;
	memset(&req, 0, sizeof(struct usb_request_t));
	write_be32(&req.signature[0], USB_REQUEST_SIGN);
	write_be32(&req.tag[0], make_tag());
	write_be32(&req.length[0], 5);
	req.flag = USB_DIRECTION_IN;
	req.cmdlen = 6;
	req.cmd.opcode = OPCODE_READ_FLASH_ID;
	usb_bulk_send(ctx->hdl, ctx->epout, &req, sizeof(struct usb_request_t));
	usb_bulk_recv(ctx->hdl, ctx->epin, &info->id[0], 5);
	usb_bulk_recv(ctx->hdl, ctx->epin, &res, sizeof(struct usb_response_t));
	if((read_be32(&res.signature[0]) != USB_RESPONSE_SIGN) || (memcmp(&res.tag[0], &req.tag[0], 4) != 0))
		return 0;
	return 1;
}

static inline int rock_flash_erase_lba_raw(struct xrock_ctx_t * ctx, uint32_t sec, uint32_t cnt)
{
	struct usb_request_t req;
	struct usb_response_t res;

	memset(&req, 0, sizeof(struct usb_request_t));
	write_be32(&req.signature[0], USB_REQUEST_SIGN);
	write_be32(&req.tag[0], make_tag());
	write_be32(&req.length[0], 0);
	req.flag = USB_DIRECTION_OUT;
	req.cmdlen = 10;
	req.cmd.opcode = OPCODE_ERASE_LBA;
	write_be32(&req.cmd.address[0], sec);
	write_be16(&req.cmd.size[0], (uint16_t)cnt);

	usb_bulk_send(ctx->hdl, ctx->epout, &req, sizeof(struct usb_request_t));
	usb_bulk_recv(ctx->hdl, ctx->epin, &res, sizeof(struct usb_response_t));
	if((read_be32(&res.signature[0]) != USB_RESPONSE_SIGN) || (memcmp(&res.tag[0], &req.tag[0], 4) != 0))
		return 0;
	return 1;
}

static inline int rock_flash_read_lba_raw(struct xrock_ctx_t * ctx, uint32_t sec, uint32_t cnt, void * buf)
{
	struct usb_request_t req;
	struct usb_response_t res;

	memset(&req, 0, sizeof(struct usb_request_t));
	write_be32(&req.signature[0], USB_REQUEST_SIGN);
	write_be32(&req.tag[0], make_tag());
	write_be32(&req.length[0], cnt << 9);
	req.flag = USB_DIRECTION_IN;
	req.cmdlen = 10;
	req.cmd.opcode = OPCODE_READ_LBA;
	req.cmd.subcode = 0;
	write_be32(&req.cmd.address[0], sec);
	write_be16(&req.cmd.size[0], (uint16_t)cnt);

	usb_bulk_send(ctx->hdl, ctx->epout, &req, sizeof(struct usb_request_t));
	usb_bulk_recv(ctx->hdl, ctx->epin, buf, cnt << 9);
	usb_bulk_recv(ctx->hdl, ctx->epin, &res, sizeof(struct usb_response_t));
	if((read_be32(&res.signature[0]) != USB_RESPONSE_SIGN) || (memcmp(&res.tag[0], &req.tag[0], 4) != 0))
		return 0;
	return 1;
}

static inline int rock_flash_write_lba_raw(struct xrock_ctx_t * ctx, uint32_t sec, uint32_t cnt, void * buf)
{
	struct usb_request_t req;
	struct usb_response_t res;

	memset(&req, 0, sizeof(struct usb_request_t));
	write_be32(&req.signature[0], USB_REQUEST_SIGN);
	write_be32(&req.tag[0], make_tag());
	write_be32(&req.length[0], cnt << 9);
	req.flag = USB_DIRECTION_OUT;
	req.cmdlen = 10;
	req.cmd.opcode = OPCODE_WRITE_LBA;
	req.cmd.subcode = 0;
	write_be32(&req.cmd.address[0], sec);
	write_be16(&req.cmd.size[0], (uint16_t)cnt);

	usb_bulk_send(ctx->hdl, ctx->epout, &req, sizeof(struct usb_request_t));
	usb_bulk_send(ctx->hdl, ctx->epout, buf, cnt << 9);
	usb_bulk_recv(ctx->hdl, ctx->epin, &res, sizeof(struct usb_response_t));
	if((read_be32(&res.signature[0]) != USB_RESPONSE_SIGN) || (memcmp(&res.tag[0], &req.tag[0], 4) != 0))
		return 0;
	return 1;
}

int rock_flash_erase_lba(struct xrock_ctx_t * ctx, uint32_t sec, uint32_t cnt)
{
	uint32_t n;

	while(cnt > 0)
	{
		n = cnt > 32768 ? 32768 : cnt;
		if(!rock_flash_erase_lba_raw(ctx, sec, n))
			return 0;
		sec += n;
		cnt -= n;
	}
	return 1;
}

int rock_flash_read_lba(struct xrock_ctx_t * ctx, uint32_t sec, uint32_t cnt, void * buf)
{
	uint32_t n;

	while(cnt > 0)
	{
		n = cnt > 128 ? 128 : cnt;
		if(!rock_flash_read_lba_raw(ctx, sec, n, buf))
			return 0;
		sec += n;
		buf += (n << 9);
		cnt -= n;
	}
	return 1;
}

int rock_flash_write_lba(struct xrock_ctx_t * ctx, uint32_t sec, uint32_t cnt, void * buf)
{
	uint32_t n;

	while(cnt > 0)
	{
		n = cnt > 128 ? 128 : cnt;
		if(!rock_flash_write_lba_raw(ctx, sec, n, buf))
			return 0;
		sec += n;
		buf += (n << 9);
		cnt -= n;
	}
	return 1;
}

int rock_flash_erase_lba_progress(struct xrock_ctx_t * ctx, uint32_t sec, uint32_t cnt)
{
	struct progress_t p;
	uint32_t n;

	progress_start(&p, (uint64_t)cnt << 9);
	while(cnt > 0)
	{
		n = cnt > 32768 ? 32768 : cnt;
		if(!rock_flash_erase_lba_raw(ctx, sec, n))
			return 0;
		sec += n;
		cnt -= n;
		progress_update(&p, (uint64_t)n << 9);
	}
	progress_stop(&p);
	return 1;
}

int rock_flash_read_lba_progress(struct xrock_ctx_t * ctx, uint32_t sec, uint32_t cnt, void * buf)
{
	struct progress_t p;
	uint32_t n;

	progress_start(&p, (uint64_t)cnt << 9);
	while(cnt > 0)
	{
		n = cnt > 128 ? 128 : cnt;
		if(!rock_flash_read_lba_raw(ctx, sec, n, buf))
			return 0;
		sec += n;
		buf += (n << 9);
		cnt -= n;
		progress_update(&p, (uint64_t)n << 9);
	}
	progress_stop(&p);
	return 1;
}

int rock_flash_write_lba_progress(struct xrock_ctx_t * ctx, uint32_t sec, uint32_t cnt, void * buf)
{
	struct progress_t p;
	uint32_t n;

	progress_start(&p, (uint64_t)cnt << 9);
	while(cnt > 0)
	{
		n = cnt > 128 ? 128 : cnt;
		if(!rock_flash_write_lba_raw(ctx, sec, n, buf))
			return 0;
		sec += n;
		buf += (n << 9);
		cnt -= n;
		progress_update(&p, (uint64_t)n << 9);
	}
	progress_stop(&p);
	return 1;
}

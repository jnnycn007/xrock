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

static inline void rock_maskrom_write_arm32(struct xrock_ctx_t * ctx, uint32_t addr, void * buf, size_t len, int rc4)
{
	static const uint8_t payload[] = {
		0x00, 0x00, 0xa0, 0xe3, 0x17, 0x0f, 0x08, 0xee, 0x15, 0x0f, 0x07, 0xee,
		0xd5, 0x0f, 0x07, 0xee, 0x9a, 0x0f, 0x07, 0xee, 0x95, 0x0f, 0x07, 0xee,
		0x28, 0x01, 0x00, 0xea, 0x00, 0x00, 0x51, 0xe1, 0x91, 0x00, 0x00, 0x3a,
		0x00, 0x00, 0xa0, 0x03, 0x0e, 0xf0, 0xa0, 0x01, 0x01, 0x40, 0x2d, 0xe9,
		0x04, 0x20, 0x52, 0xe2, 0x20, 0x00, 0x00, 0xba, 0x03, 0xc0, 0x10, 0xe2,
		0x28, 0x00, 0x00, 0x1a, 0x03, 0xc0, 0x11, 0xe2, 0x32, 0x00, 0x00, 0x1a,
		0x08, 0x20, 0x52, 0xe2, 0x12, 0x00, 0x00, 0xba, 0x14, 0x20, 0x52, 0xe2,
		0x0b, 0x00, 0x00, 0xba, 0x10, 0x00, 0x2d, 0xe9, 0x18, 0x50, 0xb1, 0xe8,
		0x18, 0x50, 0xa0, 0xe8, 0x18, 0x50, 0xb1, 0xe8, 0x18, 0x50, 0xa0, 0xe8,
		0x20, 0x20, 0x52, 0xe2, 0xf9, 0xff, 0xff, 0xaa, 0x10, 0x00, 0x72, 0xe3,
		0x18, 0x50, 0xb1, 0xa8, 0x18, 0x50, 0xa0, 0xa8, 0x10, 0x20, 0x42, 0xa2,
		0x10, 0x00, 0xbd, 0xe8, 0x14, 0x20, 0x92, 0xe2, 0x08, 0x50, 0xb1, 0xa8,
		0x08, 0x50, 0xa0, 0xa8, 0x0c, 0x20, 0x52, 0xa2, 0xfb, 0xff, 0xff, 0xaa,
		0x08, 0x20, 0x92, 0xe2, 0x05, 0x00, 0x00, 0xba, 0x04, 0x20, 0x52, 0xe2,
		0x04, 0x30, 0x91, 0xb4, 0x04, 0x30, 0x80, 0xb4, 0x08, 0x10, 0xb1, 0xa8,
		0x08, 0x10, 0xa0, 0xa8, 0x04, 0x20, 0x42, 0xa2, 0x04, 0x20, 0x92, 0xe2,
		0x01, 0x80, 0xbd, 0x08, 0x02, 0x00, 0x52, 0xe3, 0x01, 0x30, 0xd1, 0xe4,
		0x01, 0x30, 0xc0, 0xe4, 0x01, 0x30, 0xd1, 0xa4, 0x01, 0x30, 0xc0, 0xa4,
		0x01, 0x30, 0xd1, 0xc4, 0x01, 0x30, 0xc0, 0xc4, 0x01, 0x80, 0xbd, 0xe8,
		0x04, 0xc0, 0x6c, 0xe2, 0x02, 0x00, 0x5c, 0xe3, 0x01, 0x30, 0xd1, 0xe4,
		0x01, 0x30, 0xc0, 0xe4, 0x01, 0x30, 0xd1, 0xa4, 0x01, 0x30, 0xc0, 0xa4,
		0x01, 0x30, 0xd1, 0xc4, 0x01, 0x30, 0xc0, 0xc4, 0x0c, 0x20, 0x52, 0xe0,
		0xeb, 0xff, 0xff, 0xba, 0x03, 0xc0, 0x11, 0xe2, 0xcc, 0xff, 0xff, 0x0a,
		0x03, 0x10, 0xc1, 0xe3, 0x04, 0xe0, 0x91, 0xe4, 0x02, 0x00, 0x5c, 0xe3,
		0x36, 0x00, 0x00, 0xca, 0x1a, 0x00, 0x00, 0x0a, 0x0c, 0x00, 0x52, 0xe3,
		0x10, 0x00, 0x00, 0xba, 0x0c, 0x20, 0x42, 0xe2, 0x30, 0x00, 0x2d, 0xe9,
		0x2e, 0x34, 0xa0, 0xe1, 0x30, 0x50, 0xb1, 0xe8, 0x04, 0x3c, 0x83, 0xe1,
		0x24, 0x44, 0xa0, 0xe1, 0x05, 0x4c, 0x84, 0xe1, 0x25, 0x54, 0xa0, 0xe1,
		0x0c, 0x5c, 0x85, 0xe1, 0x2c, 0xc4, 0xa0, 0xe1, 0x0e, 0xcc, 0x8c, 0xe1,
		0x38, 0x10, 0xa0, 0xe8, 0x10, 0x20, 0x52, 0xe2, 0xf3, 0xff, 0xff, 0xaa,
		0x30, 0x00, 0xbd, 0xe8, 0x0c, 0x20, 0x92, 0xe2, 0x05, 0x00, 0x00, 0xba,
		0x2e, 0xc4, 0xa0, 0xe1, 0x04, 0xe0, 0x91, 0xe4, 0x0e, 0xcc, 0x8c, 0xe1,
		0x04, 0xc0, 0x80, 0xe4, 0x04, 0x20, 0x52, 0xe2, 0xf9, 0xff, 0xff, 0xaa,
		0x03, 0x10, 0x41, 0xe2, 0xc9, 0xff, 0xff, 0xea, 0x0c, 0x00, 0x52, 0xe3,
		0x10, 0x00, 0x00, 0xba, 0x0c, 0x20, 0x42, 0xe2, 0x30, 0x00, 0x2d, 0xe9,
		0x2e, 0x38, 0xa0, 0xe1, 0x30, 0x50, 0xb1, 0xe8, 0x04, 0x38, 0x83, 0xe1,
		0x24, 0x48, 0xa0, 0xe1, 0x05, 0x48, 0x84, 0xe1, 0x25, 0x58, 0xa0, 0xe1,
		0x0c, 0x58, 0x85, 0xe1, 0x2c, 0xc8, 0xa0, 0xe1, 0x0e, 0xc8, 0x8c, 0xe1,
		0x38, 0x10, 0xa0, 0xe8, 0x10, 0x20, 0x52, 0xe2, 0xf3, 0xff, 0xff, 0xaa,
		0x30, 0x00, 0xbd, 0xe8, 0x0c, 0x20, 0x92, 0xe2, 0x05, 0x00, 0x00, 0xba,
		0x2e, 0xc8, 0xa0, 0xe1, 0x04, 0xe0, 0x91, 0xe4, 0x0e, 0xc8, 0x8c, 0xe1,
		0x04, 0xc0, 0x80, 0xe4, 0x04, 0x20, 0x52, 0xe2, 0xf9, 0xff, 0xff, 0xaa,
		0x02, 0x10, 0x41, 0xe2, 0xae, 0xff, 0xff, 0xea, 0x0c, 0x00, 0x52, 0xe3,
		0x10, 0x00, 0x00, 0xba, 0x0c, 0x20, 0x42, 0xe2, 0x30, 0x00, 0x2d, 0xe9,
		0x2e, 0x3c, 0xa0, 0xe1, 0x30, 0x50, 0xb1, 0xe8, 0x04, 0x34, 0x83, 0xe1,
		0x24, 0x4c, 0xa0, 0xe1, 0x05, 0x44, 0x84, 0xe1, 0x25, 0x5c, 0xa0, 0xe1,
		0x0c, 0x54, 0x85, 0xe1, 0x2c, 0xcc, 0xa0, 0xe1, 0x0e, 0xc4, 0x8c, 0xe1,
		0x38, 0x10, 0xa0, 0xe8, 0x10, 0x20, 0x52, 0xe2, 0xf3, 0xff, 0xff, 0xaa,
		0x30, 0x00, 0xbd, 0xe8, 0x0c, 0x20, 0x92, 0xe2, 0x05, 0x00, 0x00, 0xba,
		0x2e, 0xcc, 0xa0, 0xe1, 0x04, 0xe0, 0x91, 0xe4, 0x0e, 0xc4, 0x8c, 0xe1,
		0x04, 0xc0, 0x80, 0xe4, 0x04, 0x20, 0x52, 0xe2, 0xf9, 0xff, 0xff, 0xaa,
		0x01, 0x10, 0x41, 0xe2, 0x93, 0xff, 0xff, 0xea, 0x02, 0x10, 0x81, 0xe0,
		0x02, 0x00, 0x80, 0xe0, 0x04, 0x20, 0x52, 0xe2, 0x1f, 0x00, 0x00, 0xba,
		0x03, 0xc0, 0x10, 0xe2, 0x27, 0x00, 0x00, 0x1a, 0x03, 0xc0, 0x11, 0xe2,
		0x30, 0x00, 0x00, 0x1a, 0x08, 0x20, 0x52, 0xe2, 0x11, 0x00, 0x00, 0xba,
		0x10, 0x40, 0x2d, 0xe9, 0x14, 0x20, 0x52, 0xe2, 0x05, 0x00, 0x00, 0xba,
		0x18, 0x50, 0x31, 0xe9, 0x18, 0x50, 0x20, 0xe9, 0x18, 0x50, 0x31, 0xe9,
		0x18, 0x50, 0x20, 0xe9, 0x20, 0x20, 0x52, 0xe2, 0xf9, 0xff, 0xff, 0xaa,
		0x10, 0x00, 0x72, 0xe3, 0x18, 0x50, 0x31, 0xa9, 0x18, 0x50, 0x20, 0xa9,
		0x10, 0x20, 0x42, 0xa2, 0x14, 0x20, 0x92, 0xe2, 0x08, 0x50, 0x31, 0xa9,
		0x08, 0x50, 0x20, 0xa9, 0x0c, 0x20, 0x42, 0xa2, 0x10, 0x40, 0xbd, 0xe8,
		0x08, 0x20, 0x92, 0xe2, 0x05, 0x00, 0x00, 0xba, 0x04, 0x20, 0x52, 0xe2,
		0x04, 0x30, 0x31, 0xb5, 0x04, 0x30, 0x20, 0xb5, 0x08, 0x10, 0x31, 0xa9,
		0x08, 0x10, 0x20, 0xa9, 0x04, 0x20, 0x42, 0xa2, 0x04, 0x20, 0x92, 0xe2,
		0x0e, 0xf0, 0xa0, 0x01, 0x02, 0x00, 0x52, 0xe3, 0x01, 0x30, 0x71, 0xe5,
		0x01, 0x30, 0x60, 0xe5, 0x01, 0x30, 0x71, 0xa5, 0x01, 0x30, 0x60, 0xa5,
		0x01, 0x30, 0x71, 0xc5, 0x01, 0x30, 0x60, 0xc5, 0x0e, 0xf0, 0xa0, 0xe1,
		0x02, 0x00, 0x5c, 0xe3, 0x01, 0x30, 0x71, 0xe5, 0x01, 0x30, 0x60, 0xe5,
		0x01, 0x30, 0x71, 0xa5, 0x01, 0x30, 0x60, 0xa5, 0x01, 0x30, 0x71, 0xc5,
		0x01, 0x30, 0x60, 0xc5, 0x0c, 0x20, 0x52, 0xe0, 0xec, 0xff, 0xff, 0xba,
		0x03, 0xc0, 0x11, 0xe2, 0xce, 0xff, 0xff, 0x0a, 0x03, 0x10, 0xc1, 0xe3,
		0x00, 0x30, 0x91, 0xe5, 0x02, 0x00, 0x5c, 0xe3, 0x36, 0x00, 0x00, 0xba,
		0x1a, 0x00, 0x00, 0x0a, 0x0c, 0x00, 0x52, 0xe3, 0x10, 0x00, 0x00, 0xba,
		0x0c, 0x20, 0x42, 0xe2, 0x30, 0x40, 0x2d, 0xe9, 0x03, 0xe4, 0xa0, 0xe1,
		0x38, 0x10, 0x31, 0xe9, 0x2c, 0xec, 0x8e, 0xe1, 0x0c, 0xc4, 0xa0, 0xe1,
		0x25, 0xcc, 0x8c, 0xe1, 0x05, 0x54, 0xa0, 0xe1, 0x24, 0x5c, 0x85, 0xe1,
		0x04, 0x44, 0xa0, 0xe1, 0x23, 0x4c, 0x84, 0xe1, 0x30, 0x50, 0x20, 0xe9,
		0x10, 0x20, 0x52, 0xe2, 0xf3, 0xff, 0xff, 0xaa, 0x30, 0x40, 0xbd, 0xe8,
		0x0c, 0x20, 0x92, 0xe2, 0x05, 0x00, 0x00, 0xba, 0x03, 0xc4, 0xa0, 0xe1,
		0x04, 0x30, 0x31, 0xe5, 0x23, 0xcc, 0x8c, 0xe1, 0x04, 0xc0, 0x20, 0xe5,
		0x04, 0x20, 0x52, 0xe2, 0xf9, 0xff, 0xff, 0xaa, 0x03, 0x10, 0x81, 0xe2,
		0xca, 0xff, 0xff, 0xea, 0x0c, 0x00, 0x52, 0xe3, 0x10, 0x00, 0x00, 0xba,
		0x0c, 0x20, 0x42, 0xe2, 0x30, 0x40, 0x2d, 0xe9, 0x03, 0xe8, 0xa0, 0xe1,
		0x38, 0x10, 0x31, 0xe9, 0x2c, 0xe8, 0x8e, 0xe1, 0x0c, 0xc8, 0xa0, 0xe1,
		0x25, 0xc8, 0x8c, 0xe1, 0x05, 0x58, 0xa0, 0xe1, 0x24, 0x58, 0x85, 0xe1,
		0x04, 0x48, 0xa0, 0xe1, 0x23, 0x48, 0x84, 0xe1, 0x30, 0x50, 0x20, 0xe9,
		0x10, 0x20, 0x52, 0xe2, 0xf3, 0xff, 0xff, 0xaa, 0x30, 0x40, 0xbd, 0xe8,
		0x0c, 0x20, 0x92, 0xe2, 0x05, 0x00, 0x00, 0xba, 0x03, 0xc8, 0xa0, 0xe1,
		0x04, 0x30, 0x31, 0xe5, 0x23, 0xc8, 0x8c, 0xe1, 0x04, 0xc0, 0x20, 0xe5,
		0x04, 0x20, 0x52, 0xe2, 0xf9, 0xff, 0xff, 0xaa, 0x02, 0x10, 0x81, 0xe2,
		0xaf, 0xff, 0xff, 0xea, 0x0c, 0x00, 0x52, 0xe3, 0x10, 0x00, 0x00, 0xba,
		0x0c, 0x20, 0x42, 0xe2, 0x30, 0x40, 0x2d, 0xe9, 0x03, 0xec, 0xa0, 0xe1,
		0x38, 0x10, 0x31, 0xe9, 0x2c, 0xe4, 0x8e, 0xe1, 0x0c, 0xcc, 0xa0, 0xe1,
		0x25, 0xc4, 0x8c, 0xe1, 0x05, 0x5c, 0xa0, 0xe1, 0x24, 0x54, 0x85, 0xe1,
		0x04, 0x4c, 0xa0, 0xe1, 0x23, 0x44, 0x84, 0xe1, 0x30, 0x50, 0x20, 0xe9,
		0x10, 0x20, 0x52, 0xe2, 0xf3, 0xff, 0xff, 0xaa, 0x30, 0x40, 0xbd, 0xe8,
		0x0c, 0x20, 0x92, 0xe2, 0x05, 0x00, 0x00, 0xba, 0x03, 0xcc, 0xa0, 0xe1,
		0x04, 0x30, 0x31, 0xe5, 0x23, 0xc4, 0x8c, 0xe1, 0x04, 0xc0, 0x20, 0xe5,
		0x04, 0x20, 0x52, 0xe2, 0xf9, 0xff, 0xff, 0xaa, 0x01, 0x10, 0x81, 0xe2,
		0x94, 0xff, 0xff, 0xea, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x4f, 0xe2, 0x00, 0xd0, 0x80, 0xe5,
		0x04, 0xe0, 0x80, 0xe5, 0x00, 0xe0, 0x0f, 0xe1, 0x08, 0xe0, 0x80, 0xe5,
		0x10, 0xef, 0x11, 0xee, 0x0c, 0xe0, 0x80, 0xe5, 0x10, 0xef, 0x1c, 0xee,
		0x10, 0xe0, 0x80, 0xe5, 0x10, 0xef, 0x11, 0xee, 0x14, 0xe0, 0x80, 0xe5,
		0x54, 0x00, 0x8f, 0xe2, 0x48, 0x10, 0x9f, 0xe5, 0x01, 0x00, 0x50, 0xe1,
		0x03, 0x00, 0x00, 0x0a, 0x3c, 0x00, 0x9f, 0xe5, 0x40, 0x10, 0x8f, 0xe2,
		0x38, 0x20, 0x9f, 0xe5, 0xc3, 0xfe, 0xff, 0xeb, 0x00, 0x00, 0xa0, 0xe1,
		0x70, 0x00, 0x4f, 0xe2, 0x00, 0xd0, 0x90, 0xe5, 0x04, 0xe0, 0x90, 0xe5,
		0x14, 0x10, 0x90, 0xe5, 0x10, 0x1f, 0x01, 0xee, 0x10, 0x10, 0x90, 0xe5,
		0x10, 0x1f, 0x0c, 0xee, 0x0c, 0x10, 0x90, 0xe5, 0x10, 0x1f, 0x01, 0xee,
		0x08, 0x10, 0x90, 0xe5, 0x01, 0xf0, 0x29, 0xe1, 0x1e, 0xff, 0x2f, 0xe1,
	};

	uint8_t * tmp = malloc(sizeof(payload) + 8 + len);
	if(tmp)
	{
		memcpy(&tmp[0], &payload[0], sizeof(payload));
		write_le32(tmp + sizeof(payload) + 0, addr);
		write_le32(tmp + sizeof(payload) + 4, (uint32_t)len);
		memcpy(tmp + sizeof(payload) + 8, buf, len);
		rock_maskrom_upload_memory(ctx, 0x471, tmp, sizeof(payload) + 8 + len, rc4);
		free(tmp);
	}
}

void rock_maskrom_write_arm32_progress(struct xrock_ctx_t * ctx, uint32_t addr, void * buf, size_t len, int rc4)
{
	struct progress_t p;
	size_t n;

	progress_start(&p, len);
	while(len > 0)
	{
		n = len > 1024 ? 1024 : len;
		rock_maskrom_write_arm32(ctx, addr, buf, n, rc4);
		addr += n;
		buf += n;
		len -= n;
		progress_update(&p, n);
	}
	progress_stop(&p);
}

void rock_maskrom_exec_arm32(struct xrock_ctx_t * ctx, uint32_t addr, int rc4)
{
	static uint8_t payload[] = {
		0x00, 0x00, 0xa0, 0xe3, 0x17, 0x0f, 0x08, 0xee, 0x15, 0x0f, 0x07, 0xee,
		0xd5, 0x0f, 0x07, 0xee, 0x9a, 0x0f, 0x07, 0xee, 0x95, 0x0f, 0x07, 0xee,
		0x06, 0x00, 0x00, 0xea, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x4f, 0xe2,
		0x00, 0xd0, 0x80, 0xe5, 0x04, 0xe0, 0x80, 0xe5, 0x00, 0xe0, 0x0f, 0xe1,
		0x08, 0xe0, 0x80, 0xe5, 0x10, 0xef, 0x11, 0xee, 0x0c, 0xe0, 0x80, 0xe5,
		0x10, 0xef, 0x1c, 0xee, 0x10, 0xe0, 0x80, 0xe5, 0x10, 0xef, 0x11, 0xee,
		0x14, 0xe0, 0x80, 0xe5, 0x00, 0x00, 0xa0, 0xe3, 0x00, 0x10, 0xa0, 0xe3,
		0x58, 0x20, 0x1f, 0xe5, 0x02, 0xf0, 0xa0, 0xe1, 0x5c, 0x00, 0x4f, 0xe2,
		0x00, 0xd0, 0x90, 0xe5, 0x04, 0xe0, 0x90, 0xe5, 0x14, 0x10, 0x90, 0xe5,
		0x10, 0x1f, 0x01, 0xee, 0x10, 0x10, 0x90, 0xe5, 0x10, 0x1f, 0x0c, 0xee,
		0x0c, 0x10, 0x90, 0xe5, 0x10, 0x1f, 0x01, 0xee, 0x08, 0x10, 0x90, 0xe5,
		0x01, 0xf0, 0x29, 0xe1, 0x1e, 0xff, 0x2f, 0xe1,
	};

	write_le32(&payload[0x1c], addr);
	rock_maskrom_upload_memory(ctx, 0x471, payload, sizeof(payload), rc4);
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
	OPCODE_TEST_UNIT_READY		= 0x00,
	OPCODE_READ_FLASH_ID		= 0x01,
	OPCODE_SET_DEVICE_ID		= 0x02,
	OPCODE_TEST_BAD_BLOCK		= 0x03,
	OPCODE_READ_SECTOR			= 0x04,
	OPCODE_WRITE_SECTOR			= 0x05,
	OPCODE_ERASE_NORMAL			= 0x06,
	OPCODE_WRITE_SPARE			= 0x07,
	OPCODE_READ_SPARE			= 0x08,
	OPCODE_ERASE_FORCE			= 0x0b,
	OPCODE_GET_VERSION			= 0x0c,
	OPCODE_READ_LBA				= 0x14,
	OPCODE_WRITE_LBA			= 0x15,
	OPCODE_ERASE_SYSDISK		= 0x16,
	OPCODE_READ_SDRAM			= 0x17,
	OPCODE_WRITE_SDRAM			= 0x18,
	OPCODE_EXEC_SDRAM			= 0x19,
	OPCODE_READ_FLASH_INFO		= 0x1a,
	OPCODE_READ_CHIP_INFO		= 0x1b,
	OPCODE_LOW_FORMAT			= 0x1c,
	OPCODE_SET_RESET_FLAG		= 0x1e,
	OPCODE_WRITE_EFUSE			= 0x1f,
	OPCODE_READ_EFUSE			= 0x20,
	OPCODE_READ_SPI_FLASH		= 0x21,
	OPCODE_WRITE_SPI_FLASH		= 0x22,
	OPCODE_WRITE_NEW_EFUSE		= 0x23,
	OPCODE_READ_NEW_EFUSE		= 0x24,
	OPCODE_ERASE_LBA			= 0x25,
	OPCODE_WRITE_VENDOR_STORAGE	= 0x26,
	OPCODE_READ_VENDOR_STORAGE	= 0x27,
	OPCODE_READ_COM_LOG			= 0x28,
	OPCODE_SWITCH_STORAGE		= 0x2a,
	OPCODE_READ_STORAGE			= 0x2b,
	OPCODE_READ_OTP_CHIP		= 0x2c,
	OPCODE_SESSION				= 0x30,
	OPCODE_READ_CAPABILITY		= 0xaa,
	OPCODE_SWITCH_USB3			= 0xbb,
	OPCODE_RESET_DEVICE			= 0xff,
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
	write_le32(&req.length[0], 0);
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
	write_le32(&req.length[0], 16);
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
	write_le32(&req.length[0], 8);
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

int rock_capability_support(struct xrock_ctx_t * ctx, enum capability_type_t type)
{
	uint8_t buf[8] = { 0 };

	if(rock_capability(ctx, buf))
	{
		switch(type)
		{
		case CAPABILITY_TYPE_DIRECT_LBA:
			return (buf[0] & (1 << 0)) ? 1 : 0;
		case CAPABILITY_TYPE_VENDOR_STORAGE:
			return (buf[0] & (1 << 1)) ? 1 : 0;
		case CAPABILITY_TYPE_FIRST_4M_ACCESS:
			return (buf[0] & (1 << 2)) ? 1 : 0;
		case CAPABILITY_TYPE_READ_LBA:
			return (buf[0] & (1 << 3)) ? 1 : 0;
		case CAPABILITY_TYPE_NEW_VENDOR_STORAGE:
			return (buf[0] & (1 << 4)) ? 1 : 0;
		case CAPABILITY_TYPE_READ_COM_LOG:
			return (buf[0] & (1 << 5)) ? 1 : 0;
		case CAPABILITY_TYPE_READ_IDB_CONFIG:
			return (buf[0] & (1 << 6)) ? 1 : 0;
		case CAPABILITY_TYPE_READ_SECURE_MODE:
			return (buf[0] & (1 << 7)) ? 1 : 0;
		case CAPABILITY_TYPE_NEW_IDB:
			return (buf[1] & (1 << 0)) ? 1 : 0;
		case CAPABILITY_TYPE_SWITCH_STORAGE:
			return (buf[1] & (1 << 1)) ? 1 : 0;
		case CAPABILITY_TYPE_LBA_PARITY:
			return (buf[1] & (1 << 2)) ? 1 : 0;
		case CAPABILITY_TYPE_READ_OTP_CHIP:
			return (buf[1] & (1 << 3)) ? 1 : 0;
		case CAPABILITY_TYPE_SWITCH_USB3:
			return (buf[1] & (1 << 4)) ? 1 : 0;
		default:
			break;
		}
	}
	return 0;
}

int rock_reset(struct xrock_ctx_t * ctx, int maskrom)
{
	struct usb_request_t req;
	struct usb_response_t res;

	memset(&req, 0, sizeof(struct usb_request_t));
	write_be32(&req.signature[0], USB_REQUEST_SIGN);
	write_be32(&req.tag[0], make_tag());
	write_le32(&req.length[0], 0);
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
	write_le32(&req.length[0], 0);
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
	write_le32(&req.length[0], 0);
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
	write_le32(&req.length[0], 0);
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

int rock_otp_read(struct xrock_ctx_t * ctx, uint8_t * buf, int len)
{
	struct usb_request_t req;
	struct usb_response_t res;

	memset(&req, 0, sizeof(struct usb_request_t));
	write_be32(&req.signature[0], USB_REQUEST_SIGN);
	write_be32(&req.tag[0], make_tag());
	write_le32(&req.length[0], len);
	req.flag = USB_DIRECTION_IN;
	req.cmdlen = 6;
	req.cmd.opcode = OPCODE_READ_OTP_CHIP;
	usb_bulk_send(ctx->hdl, ctx->epout, &req, sizeof(struct usb_request_t));
	usb_bulk_recv(ctx->hdl, ctx->epin, buf, len);
	usb_bulk_recv(ctx->hdl, ctx->epin, &res, sizeof(struct usb_response_t));
	if((read_be32(&res.signature[0]) != USB_RESPONSE_SIGN) || (memcmp(&res.tag[0], &req.tag[0], 4) != 0))
		return 0;
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

int rock_vs_read(struct xrock_ctx_t * ctx, int type, int index, uint8_t * buf, int len)
{
	struct usb_request_t req;
	struct usb_response_t res;

	memset(&req, 0, sizeof(struct usb_request_t));
	write_be32(&req.signature[0], USB_REQUEST_SIGN);
	write_be32(&req.tag[0], make_tag());
	write_le32(&req.length[0], len);
	req.flag = USB_DIRECTION_IN;
	req.cmdlen = 10;
	req.cmd.opcode = OPCODE_READ_VENDOR_STORAGE;
	req.cmd.subcode = 0;
	write_be16(&req.cmd.address[0], index);
	write_be16(&req.cmd.address[2], type);
	write_be16(&req.cmd.size[0], len);

	usb_bulk_send(ctx->hdl, ctx->epout, &req, sizeof(struct usb_request_t));
	usb_bulk_recv(ctx->hdl, ctx->epin, buf, len);
	usb_bulk_recv(ctx->hdl, ctx->epin, &res, sizeof(struct usb_response_t));
	if((read_be32(&res.signature[0]) != USB_RESPONSE_SIGN) || (memcmp(&res.tag[0], &req.tag[0], 4) != 0))
		return 0;
	return 1;
}

int rock_vs_write(struct xrock_ctx_t * ctx, int type, int index, uint8_t * buf, int len)
{
	struct usb_request_t req;
	struct usb_response_t res;

	memset(&req, 0, sizeof(struct usb_request_t));
	write_be32(&req.signature[0], USB_REQUEST_SIGN);
	write_be32(&req.tag[0], make_tag());
	write_le32(&req.length[0], len);
	req.flag = USB_DIRECTION_OUT;
	req.cmdlen = 10;
	req.cmd.opcode = OPCODE_WRITE_VENDOR_STORAGE;
	req.cmd.subcode = 0;
	write_be16(&req.cmd.address[0], index);
	write_be16(&req.cmd.address[2], type);
	write_be16(&req.cmd.size[0], len);

	usb_bulk_send(ctx->hdl, ctx->epout, &req, sizeof(struct usb_request_t));
	usb_bulk_send(ctx->hdl, ctx->epout, buf, len);
	usb_bulk_recv(ctx->hdl, ctx->epin, &res, sizeof(struct usb_response_t));
	if((read_be32(&res.signature[0]) != USB_RESPONSE_SIGN) || (memcmp(&res.tag[0], &req.tag[0], 4) != 0))
		return 0;
	return 1;
}

enum storage_type_t rock_storage_read(struct xrock_ctx_t * ctx)
{
	struct usb_request_t req;
	struct usb_response_t res;
	uint8_t buf[4];

	memset(&req, 0, sizeof(struct usb_request_t));
	write_be32(&req.signature[0], USB_REQUEST_SIGN);
	write_be32(&req.tag[0], make_tag());
	write_le32(&req.length[0], 4);
	req.flag = USB_DIRECTION_IN;
	req.cmdlen = 6;
	req.cmd.opcode = OPCODE_READ_STORAGE;

	usb_bulk_send(ctx->hdl, ctx->epout, &req, sizeof(struct usb_request_t));
	usb_bulk_recv(ctx->hdl, ctx->epin, buf, 4);
	usb_bulk_recv(ctx->hdl, ctx->epin, &res, sizeof(struct usb_response_t));
	if((read_be32(&res.signature[0]) != USB_RESPONSE_SIGN) || (memcmp(&res.tag[0], &req.tag[0], 4) != 0))
		return STORAGE_TYPE_UNKNOWN;
	enum storage_type_t type = (enum storage_type_t)read_le32(buf);
	switch(type)
	{
	case STORAGE_TYPE_FLASH:
	case STORAGE_TYPE_EMMC:
	case STORAGE_TYPE_SD:
	case STORAGE_TYPE_SD1:
	case STORAGE_TYPE_SPINOR:
	case STORAGE_TYPE_SPINAND:
	case STORAGE_TYPE_RAM:
	case STORAGE_TYPE_USB:
	case STORAGE_TYPE_SATA:
	case STORAGE_TYPE_PCIE:
		break;
	default:
		type = STORAGE_TYPE_UNKNOWN;
		break;
	}
	return type;
}

int rock_storage_switch(struct xrock_ctx_t * ctx, enum storage_type_t type)
{
	struct usb_request_t req;
	struct usb_response_t res;

	memset(&req, 0, sizeof(struct usb_request_t));
	write_be32(&req.signature[0], USB_REQUEST_SIGN);
	write_be32(&req.tag[0], make_tag());
	write_le32(&req.length[0], 0);
	req.flag = USB_DIRECTION_OUT;
	req.cmdlen = 6;
	req.cmd.opcode = OPCODE_SWITCH_STORAGE;
	switch(type)
	{
	case STORAGE_TYPE_FLASH:
		req.cmd.subcode = 0;
		break;
	case STORAGE_TYPE_EMMC:
		req.cmd.subcode = 1;
		break;
	case STORAGE_TYPE_SD:
		req.cmd.subcode = 2;
		break;
	case STORAGE_TYPE_SD1:
		req.cmd.subcode = 3;
		break;
	case STORAGE_TYPE_SPINOR:
		req.cmd.subcode = 9;
		break;
	case STORAGE_TYPE_SPINAND:
		req.cmd.subcode = 8;
		break;
	case STORAGE_TYPE_RAM:
		req.cmd.subcode = 6;
		break;
	case STORAGE_TYPE_USB:
		req.cmd.subcode = 7;
		break;
	case STORAGE_TYPE_SATA:
		req.cmd.subcode = 10;
		break;
	case STORAGE_TYPE_PCIE:
		req.cmd.subcode = 11;
		break;
	default:
		break;
	}

	usb_bulk_send(ctx->hdl, ctx->epout, &req, sizeof(struct usb_request_t));
	usb_bulk_recv(ctx->hdl, ctx->epin, &res, sizeof(struct usb_response_t));
	if((read_be32(&res.signature[0]) != USB_RESPONSE_SIGN) || (memcmp(&res.tag[0], &req.tag[0], 4) != 0))
		return 0;
	return 1;
}

int rock_flash_detect(struct xrock_ctx_t * ctx, struct flash_info_t * info)
{
	struct usb_request_t req;
	struct usb_response_t res;

	memset(&req, 0, sizeof(struct usb_request_t));
	write_be32(&req.signature[0], USB_REQUEST_SIGN);
	write_be32(&req.tag[0], make_tag());
	write_le32(&req.length[0], 11);
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
	write_le32(&req.length[0], 5);
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
	write_le32(&req.length[0], 0);
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
	write_le32(&req.length[0], cnt << 9);
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
	write_le32(&req.length[0], cnt << 9);
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
		n = cnt > 16384 ? 16384 : cnt;
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
		n = cnt > 16384 ? 16384 : cnt;
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
		n = cnt > 16384 ? 16384 : cnt;
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
	int MAXSEC = 16384;
	struct progress_t p;
	uint32_t n;

	if(cnt <= 65536)
		MAXSEC = 128;

	progress_start(&p, (uint64_t)cnt << 9);
	while(cnt > 0)
	{
		n = cnt > MAXSEC ? MAXSEC : cnt;
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
	int MAXSEC = 16384;
	struct progress_t p;
	uint32_t n;

	if(cnt <= 65536)
		MAXSEC = 128;

	progress_start(&p, (uint64_t)cnt << 9);
	while(cnt > 0)
	{
		n = cnt > MAXSEC ? MAXSEC : cnt;
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
	int MAXSEC = 16384;
	struct progress_t p;
	uint32_t n;

	if(cnt <= 65536)
		MAXSEC = 128;

	progress_start(&p, (uint64_t)cnt << 9);
	while(cnt > 0)
	{
		n = cnt > MAXSEC ? MAXSEC : cnt;
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

int rock_flash_read_lba_to_file_progress(struct xrock_ctx_t * ctx, uint32_t sec, uint32_t cnt, const char * filename)
{
	int MAXSEC = 16384;

	FILE * f = fopen(filename, "w");
	if(!f)
		return 0;

	if(cnt <= 65536)
		MAXSEC = 128;

	void * buf = malloc(MAXSEC << 9);
	if(!buf)
	{
		fclose(f);
		return 0;
	}

	struct progress_t p;
	progress_start(&p, (uint64_t)cnt << 9);
	while(cnt > 0)
	{
		uint32_t n = cnt > MAXSEC ? MAXSEC : cnt;
		memset(buf, 0, MAXSEC << 9);
		if(!rock_flash_read_lba_raw(ctx, sec, n, buf))
		{
			if(buf)
				free(buf);
			if(f)
				fclose(f);
			return 0;
		}
		if(fwrite(buf, 512, n, f) != n)
		{
			if(buf)
				free(buf);
			if(f)
				fclose(f);
			return 0;
		}
		sec += n;
		cnt -= n;
		progress_update(&p, (uint64_t)n << 9);
	}
	progress_stop(&p);

	free(buf);
	fclose(f);
	return 1;
}

int rock_flash_write_lba_from_file_progress(struct xrock_ctx_t * ctx, uint32_t sec, uint32_t maxcnt, const char * filename)
{
	int MAXSEC = 16384;

	FILE * f = fopen(filename, "r");
	if(!f)
		return 0;

	fseek(f, 0, SEEK_END);
	int64_t len = ftell(f);
	if(len <= 0)
	{
		fclose(f);
		return 0;
	}
	fseek(f, 0, SEEK_SET);

	uint32_t cnt = (len >> 9);
	if(len % 512 != 0)
		cnt += 1;
	if(cnt <= 0)
		cnt = maxcnt - sec;
	else if(cnt > maxcnt - sec)
		cnt = maxcnt - sec;

	if(cnt <= 65536)
		MAXSEC = 128;

	void * buf = malloc(MAXSEC << 9);
	if(!buf)
	{
		fclose(f);
		return 0;
	}

	struct progress_t p;
	progress_start(&p, (uint64_t)cnt << 9);
	while(cnt > 0)
	{
		uint32_t n = cnt > MAXSEC ? MAXSEC : cnt;
		memset(buf, 0, MAXSEC << 9);
		if(fread(buf, 512, n, f) != n)
		{
			if(buf)
				free(buf);
			if(f)
				fclose(f);
			return 0;
		}
		if(!rock_flash_write_lba_raw(ctx, sec, n, buf))
		{
			if(buf)
				free(buf);
			if(f)
				fclose(f);
			return 0;
		}
		sec += n;
		cnt -= n;
		progress_update(&p, (uint64_t)n << 9);
	}
	progress_stop(&p);

	free(buf);
	fclose(f);
	return 1;
}

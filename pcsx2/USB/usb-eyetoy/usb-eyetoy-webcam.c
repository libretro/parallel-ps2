/* SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team */
/* SPDX-License-Identifier: GPL-3.0+ */

#include "videodev.h"
#include "usb-eyetoy-webcam.h"
#include "eyetoy.h"
#include "ov519.h"
#include "cam-frame.h"
#include "USB/libretro-usb/USBinternal.h"
#include "USB/libretro-usb/desc.h"

#include <stdlib.h>
#include <string.h>

typedef struct EYETOYState
{
	USBDevice dev;
	USBDesc desc;
	USBDescDevice desc_dev;

	u32 port;
	u32 subtype;

	eyetoy_video_device* videodev;
	eyetoy_mic* mic;
	/* 255 each, as save states have always held them: register 0xFF is
	 * past the end, read as 0 and written nowhere. */
	u8 regs[0xFF];     /* OV519 */
	u8 i2c_regs[0xFF]; /* OV764x */

	int hw_camera_running;
	int frame_step;
	u8* mpeg_frame_data;
	unsigned int mpeg_frame_size;
	unsigned int mpeg_frame_offset;
} EYETOYState;

static const USBDescStrings desc_strings = {
	"",
	"Sony corporation",
	"EyeToy USB camera Namtai",
};

static u32 min_u32(u32 a, u32 b)
{
	return a < b ? a : b;
}

/* A register a vendor request names, if the register file has it. */
static u8* reg_at(EYETOYState* s, int index)
{
	const u32 i = (u32)index & 0xFF;
	return i < sizeof(s->regs) ? &s->regs[i] : NULL;
}

static void reset_controller(EYETOYState* s)
{
	if (s->subtype == EYETOY_TYPE_EYETOY)
		memcpy(s->regs, ov519_defaults, sizeof(s->regs));
	else if (s->subtype == EYETOY_TYPE_OV511P)
		memcpy(s->regs, ov511p_defaults, sizeof(s->regs));
}

static void reset_sensor(EYETOYState* s)
{
	if (s->subtype == EYETOY_TYPE_EYETOY)
		memcpy(s->i2c_regs, ov7648_defaults, sizeof(s->i2c_regs));
	else if (s->subtype == EYETOY_TYPE_OV511P)
		memcpy(s->i2c_regs, ov7620_defaults, sizeof(s->i2c_regs));
}

static int clamp_int(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static void open_camera(EYETOYState* s)
{
	if (s->hw_camera_running && s->subtype == EYETOY_TYPE_EYETOY)
	{
		/* The game writes the size; the frame buffers hold 640x480. */
		const int width = clamp_int(s->regs[OV519_R10_H_SIZE] << 4, 16, 640);
		const int height = clamp_int(s->regs[OV519_R11_V_SIZE] << 3, 8, 480);
		const enum eyetoy_frame_format format = s->regs[OV519_RA0_FORMAT] == OV519_RA0_FORMAT_JPEG ? EYETOY_FORMAT_JPEG : EYETOY_FORMAT_MPEG;
		const int mirror = !!(s->i2c_regs[OV7610_REG_COM_A] & OV7610_REG_COM_A_MASK_MIRROR);
		log_cb(RETRO_LOG_INFO,
			"EyeToy : eyetoy_open(); hw=%d, w=%d, h=%d, fmt=%d, mirr=%d\n", s->hw_camera_running, width, height, (int)format, mirror);
		if (s->videodev->ops->open(s->videodev, width, height, format, mirror) != 0)
			log_cb(RETRO_LOG_ERROR, "(Eyetoy) Failed to open video device\n");
	}
	else if (s->hw_camera_running && s->subtype == EYETOY_TYPE_OV511P)
	{
		const int width = 320;
		const int height = 240;
		log_cb(RETRO_LOG_INFO,
			"EyeToy : eyetoy_open(); hw=%d, w=%d, h=%d, fmt=%d, mirr=%d\n", s->hw_camera_running, width, height, (int)EYETOY_FORMAT_YUV400, 0);
		if (s->videodev->ops->open(s->videodev, width, height, EYETOY_FORMAT_YUV400, 0) != 0)
			log_cb(RETRO_LOG_ERROR, "(Eyetoy) Failed to open video device\n");
	}
}

static void close_camera(EYETOYState* s)
{
	log_cb(RETRO_LOG_INFO, "EyeToy : eyetoy_close(); hw=%d\n", s->hw_camera_running);
	if (s->hw_camera_running)
	{
		s->hw_camera_running = 0;
		s->videodev->ops->close(s->videodev);
	}
}

static void eyetoy_handle_reset(USBDevice* dev)
{
	EYETOYState* s = USB_CONTAINER_OF(dev, EYETOYState, dev);
	reset_controller(s);
	reset_sensor(s);
}

static void webcam_handle_control_eyetoy(USBDevice* dev, USBPacket* p, int request, int value, int index, int length, u8* data)
{
	EYETOYState* s = USB_CONTAINER_OF(dev, EYETOYState, dev);
	u8* reg;

	if (usb_desc_handle_control(dev, p, request, value, index, length, data) >= 0)
		return;

	switch (request)
	{
		case VendorDeviceRequest | 0x1: /* Read register */
			reg = reg_at(s, index);
			data[0] = reg ? *reg : 0;
			p->actual_length = 1;
			break;

		case VendorDeviceOutRequest | 0x1: /* Write register */
			switch (index)
			{
				case OV519_RA0_FORMAT:
					if (data[0] == OV519_RA0_FORMAT_MPEG)
						log_cb(RETRO_LOG_INFO, "EyeToy : configured for MPEG format\n");
					else if (data[0] == OV519_RA0_FORMAT_JPEG)
						log_cb(RETRO_LOG_INFO, "EyeToy : configured for JPEG format\n");
					else
						log_cb(RETRO_LOG_INFO, "EyeToy : configured for unknown format\n");

					if (s->hw_camera_running && s->regs[OV519_RA0_FORMAT] != data[0])
					{
						log_cb(RETRO_LOG_INFO, "EyeToy : reinitialize the camera\n");
						close_camera(s);
						open_camera(s);
					}
					break;
				case OV519_R10_H_SIZE:
					log_cb(RETRO_LOG_INFO, "EyeToy : Image width : %d\n", data[0] << 4);
					break;
				case OV519_R11_V_SIZE:
					log_cb(RETRO_LOG_INFO, "EyeToy : Image height : %d\n", data[0] << 3);
					break;
				case OV519_GPIO_DATA_OUT0:
				{
					static int led_state = -1;
					if (led_state != data[0])
					{
						led_state = data[0];
						log_cb(RETRO_LOG_INFO, "EyeToy : LED : %d\n", !!led_state);
					}
				}
				break;
				case R518_I2C_CTL:
					if (data[0] == 1) /* Commit I2C write */
					{
						const u8 i2c = s->regs[R51x_I2C_SADDR_3];
						const u8 val = s->regs[R51x_I2C_DATA];
						if ((i2c == 0x12) && (val & 0x80))
						{
							s->i2c_regs[0x12] = val & ~0x80;
							reset_sensor(s);
						}
						else if (i2c < sizeof(s->i2c_regs))
						{
							s->i2c_regs[i2c] = val;
						}
						if (i2c == OV7610_REG_COM_A)
						{
							const bool mirroring_enabled = (val & OV7610_REG_COM_A_MASK_MIRROR) != 0;
							s->videodev->ops->set_mirroring(s->videodev, mirroring_enabled);
							log_cb(RETRO_LOG_INFO, "EyeToy : mirroring %s\n", mirroring_enabled ? "ON" : "OFF");
						}
					}
					else if (s->regs[R518_I2C_CTL] == 0x03 && data[0] == 0x05)
					{
						/* the read address seems to default to 0x43 (R51x_I2C_SADDR_2) */
						const u8 i2c_reg = s->regs[R51x_I2C_SADDR_2];
						s->regs[R51x_I2C_DATA] = 0;
						if (i2c_reg < sizeof(s->i2c_regs))
							s->regs[R51x_I2C_DATA] = s->i2c_regs[i2c_reg];
					}
					break;
				default:
					break;
			}

			reg = reg_at(s, index);
			if (reg)
				*reg = data[0];
			p->actual_length = 1;
			break;
		default:
			p->status = USB_RET_STALL;
			break;
	}
}

static void webcam_handle_control_ov511p(USBDevice* dev, USBPacket* p, int request, int value, int index, int length, u8* data)
{
	EYETOYState* s = USB_CONTAINER_OF(dev, EYETOYState, dev);
	u8* reg;

	if (usb_desc_handle_control(dev, p, request, value, index, length, data) >= 0)
		return;

	switch (request)
	{
		case VendorDeviceRequest | 0x3: /* Read register */
			reg = reg_at(s, index);
			data[0] = reg ? *reg : 0;
			p->actual_length = 1;
			break;

		case VendorDeviceOutRequest | 0x2: /* Write register */
			switch (index)
			{
				case R511_I2C_CTL:
					if (data[0] == 1)
					{
						const u8 i2c = s->regs[R51x_I2C_SADDR_3];
						const u8 val = s->regs[R51x_I2C_DATA];
						if (i2c < sizeof(s->i2c_regs))
							s->i2c_regs[i2c] = val;
					}
					else if (s->regs[R511_I2C_CTL] == 0x03 && data[0] == 0x05)
					{
						const u8 i2c_reg = s->regs[R51x_I2C_SADDR_2];
						s->regs[R51x_I2C_DATA] = 0;
						if (i2c_reg < sizeof(s->i2c_regs))
							s->regs[R51x_I2C_DATA] = s->i2c_regs[i2c_reg];
					}
					break;
				default:
					break;
			}

			reg = reg_at(s, index);
			if (reg)
				*reg = data[0];
			p->actual_length = 1;
			break;
		default:
			p->status = USB_RET_STALL;
			break;
	}
}

/* A frame's first packet: header, then as much of the frame as the
 * packet has room for beyond the header and the trailing bytes it keeps
 * free. The game picks the packet's size, and one smaller than the header
 * gets what of the header fits rather than having room counted below
 * zero. Returns how much of the frame went. */
static u32 send_frame_start(EYETOYState* s, USBPacket* p, u8* data, u32 max_ep_size, const u8* header, u32 header_size, u32 keep_free)
{
	const u32 room = min_u32((u32)p->buffer_size, max_ep_size);
	const u32 data_pk = room > header_size + keep_free ? min_u32(room - header_size - keep_free, s->mpeg_frame_size) : 0;
	memcpy(data, header, header_size);
	memcpy(data + header_size, s->mpeg_frame_data, data_pk);
	usb_packet_copy(p, data, min_u32(room, header_size + data_pk));
	return data_pk;
}

/* The rest of a frame, a packet at a time, keeping keep_free bytes of the
 * packet free. */
static void send_frame_rest(EYETOYState* s, USBPacket* p, u32 keep_free)
{
	const u32 room = (u32)p->buffer_size > keep_free ? (u32)p->buffer_size - keep_free : 0;
	const u32 data_pk = min_u32(s->mpeg_frame_size - s->mpeg_frame_offset, room);
	usb_packet_copy(p, s->mpeg_frame_data + s->mpeg_frame_offset, data_pk);
	s->mpeg_frame_offset += data_pk;
	s->frame_step++;
}

#define EYETOY_MAX_EP_SIZE 896
#define OV511P_MAX_EP_SIZE 961

static void webcam_handle_data_eyetoy(USBDevice* dev, USBPacket* p)
{
	EYETOYState* s = USB_CONTAINER_OF(dev, EYETOYState, dev);
	const u8 devep = p->ep->nr;
	u8 data[EYETOY_MAX_EP_SIZE];

	if (!s->hw_camera_running)
	{
		log_cb(RETRO_LOG_INFO, "EyeToy : initialization done; start the camera\n");
		s->hw_camera_running = 1;
		open_camera(s);
	}

	switch (p->pid)
	{
		case USB_TOKEN_IN:
			if (devep == 1)
			{
				if (s->frame_step == 0)
				{
					u8 header[] = {0xFF, 0xFF, 0xFF, 0x50, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
					const int got = s->videodev->ops->get_image(s->videodev, s->mpeg_frame_data, EYETOY_MAX_FRAME_SIZE);
					s->mpeg_frame_size = got > 0 ? (unsigned int)got : 0;
					if (s->mpeg_frame_size == 0)
					{
						p->status = USB_RET_NAK;
						break;
					}
					header[0x0A] = s->regs[OV519_RA0_FORMAT] == OV519_RA0_FORMAT_JPEG ? 0x03 : 0x01;
					s->mpeg_frame_offset = send_frame_start(s, p, data, EYETOY_MAX_EP_SIZE, header, sizeof(header), 0);
					s->frame_step++;
				}
				else if (s->mpeg_frame_offset < s->mpeg_frame_size)
					send_frame_rest(s, p, 0);
				else
				{
					u8 footer[] = {0xFF, 0xFF, 0xFF, 0x51, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
					footer[0x0A] = s->regs[OV519_RA0_FORMAT] == OV519_RA0_FORMAT_JPEG ? 0x03 : 0x01;
					usb_packet_copy(p, footer, min_u32((u32)p->buffer_size, sizeof(footer)));
					s->frame_step = 0;
				}
			}
			else if (devep == 2)
			{
				/* One millisecond of 16 kHz mono per packet. */
				s16 samples[16];
				const u32 n = s->mic ? s->mic->ops->read(s->mic, samples, min_u32((u32)p->buffer_size / 2, 16)) : 0;
				usb_packet_copy(p, samples, n * 2);
			}
			break;
		case USB_TOKEN_OUT:
		default:
			p->status = USB_RET_STALL;
			break;
	}
}

static void webcam_handle_data_ov511p(USBDevice* dev, USBPacket* p)
{
	static const u8 header[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x28}; /* 28 <> 29 */
	static const u8 footer[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xA8, 0x09, 0x07};
	EYETOYState* s = USB_CONTAINER_OF(dev, EYETOYState, dev);
	const u8 devep = p->ep->nr;
	u8 data[OV511P_MAX_EP_SIZE];

	if (!s->hw_camera_running)
	{
		log_cb(RETRO_LOG_INFO, "EyeToy : initialization done; start the camera\n");
		s->hw_camera_running = 1;
		open_camera(s);
	}

	switch (p->pid)
	{
		case USB_TOKEN_IN:
			if (devep == 1)
			{
				if (s->frame_step == 0)
				{
					const int got = s->videodev->ops->get_image(s->videodev, s->mpeg_frame_data, EYETOY_MAX_FRAME_SIZE);
					s->mpeg_frame_size = got > 0 ? (unsigned int)got : 0;
					if (s->mpeg_frame_size == 0)
					{
						p->status = USB_RET_NAK;
						break;
					}
					s->mpeg_frame_offset = send_frame_start(s, p, data, OV511P_MAX_EP_SIZE, header, sizeof(header), 1);
					s->frame_step++;
				}
				else if (s->mpeg_frame_offset < s->mpeg_frame_size)
					send_frame_rest(s, p, 1);
				else
				{
					usb_packet_copy(p, (void*)footer, min_u32((u32)p->buffer_size, sizeof(footer)));
					s->frame_step = 0;
				}
			}
			break;
		case USB_TOKEN_OUT:
		default:
			p->status = USB_RET_STALL;
			break;
	}
}

static void eyetoy_handle_destroy(USBDevice* dev)
{
	EYETOYState* s = USB_CONTAINER_OF(dev, EYETOYState, dev);
	close_camera(s);
	usb_desc_device_free(&s->desc_dev);
	if (s->videodev)
		s->videodev->ops->destroy(s->videodev);
	if (s->mic)
		s->mic->ops->destroy(s->mic);
	free(s->mpeg_frame_data);
	free(s);
}

void usb_eyetoy_freeze(USBDevice* dev, UsbState* st)
{
	EYETOYState* s = USB_CONTAINER_OF(dev, EYETOYState, dev);
	usb_state_bytes(st, s->regs, sizeof(s->regs));
	usb_state_bytes(st, s->i2c_regs, sizeof(s->i2c_regs));
	if (st->reading)
	{
		/* The frame in flight is not kept: the camera starts afresh on the
		 * next request, with the format and size the registers now hold. */
		close_camera(s);
		s->frame_step = 0;
		s->mpeg_frame_size = 0;
		s->mpeg_frame_offset = 0;
	}
}

USBDevice* usb_eyetoy_create(int model)
{
	const u32 subtype = model == USB_EYETOY_MODEL_EYETOY ? EYETOY_TYPE_EYETOY : EYETOY_TYPE_OV511P;
	eyetoy_video_device* videodev = eyetoy_video_create();
	EYETOYState* s;
	if (!videodev)
	{
		log_cb(RETRO_LOG_ERROR, "EyeToy: failed to create video device\n");
		return NULL;
	}

	s = (EYETOYState*)calloc(1, sizeof(*s));
	if (!s)
	{
		videodev->ops->destroy(videodev);
		return NULL;
	}
	s->videodev = videodev;
	s->subtype = subtype;
	s->desc.full = &s->desc_dev;
	s->desc.str = desc_strings;
	s->mpeg_frame_data = (u8*)calloc(1, EYETOY_MAX_FRAME_SIZE);
	if (!s->mpeg_frame_data)
		goto fail;

	if (subtype == EYETOY_TYPE_EYETOY)
	{
		if (usb_desc_parse_dev(eyetoy_dev_descriptor, sizeof(eyetoy_dev_descriptor), &s->desc, &s->desc_dev) < 0)
			goto fail;
		if (usb_desc_parse_config(eyetoy_config_descriptor, sizeof(eyetoy_config_descriptor), &s->desc_dev) < 0)
			goto fail;
		s->dev.klass.handle_control = webcam_handle_control_eyetoy;
		s->dev.klass.handle_data = webcam_handle_data_eyetoy;

		s->mic = eyetoy_mic_create();
	}
	else
	{
		if (usb_desc_parse_dev(ov511p_dev_descriptor, sizeof(ov511p_dev_descriptor), &s->desc, &s->desc_dev) < 0)
			goto fail;
		if (usb_desc_parse_config(ov511p_config_descriptor, sizeof(ov511p_config_descriptor), &s->desc_dev) < 0)
			goto fail;
		s->dev.klass.handle_control = webcam_handle_control_ov511p;
		s->dev.klass.handle_data = webcam_handle_data_ov511p;
	}

	s->dev.speed = USB_SPEED_FULL;
	s->dev.klass.handle_attach = usb_desc_attach;
	s->dev.klass.handle_reset = eyetoy_handle_reset;
	s->dev.klass.unrealize = eyetoy_handle_destroy;
	s->dev.klass.usb_desc = &s->desc;
	s->dev.klass.product_desc = s->desc.str[2];

	usb_desc_init(&s->dev);
	usb_ep_init(&s->dev);
	eyetoy_handle_reset(&s->dev);

	s->hw_camera_running = 0;
	s->frame_step = 0;
	s->mpeg_frame_offset = 0;

	return &s->dev;
fail:
	eyetoy_handle_destroy(&s->dev);
	return NULL;
}

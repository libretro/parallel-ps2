// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "videodev.h"
#include "usb-eyetoy-webcam.h"
#include "eyetoy.h"
#include "ov519.h"
#include "USB/libretro-usb/USBinternal.h"
#include "USB/libretro-usb/desc.h"

#include <algorithm>
#include <cstring>

namespace usb_eyetoy
{
	typedef struct EYETOYState
	{
		USBDevice dev;
		USBDesc desc;
		USBDescDevice desc_dev;

		u32 port;
		u32 subtype;

		std::unique_ptr<VideoDevice> videodev;
		std::unique_ptr<MicSource> mic;
		u8 regs[0xFF]; //OV519
		u8 i2c_regs[0xFF]; //OV764x

		int hw_camera_running;
		int frame_step;
		std::unique_ptr<unsigned char[]> mpeg_frame_data;
		unsigned int mpeg_frame_size;
		unsigned int mpeg_frame_offset;
	} EYETOYState;

	static const USBDescStrings desc_strings = {
		"",
		"Sony corporation",
		"EyeToy USB camera Namtai",
	};

	static void reset_controller(EYETOYState* s)
	{
		if (s->subtype == TYPE_EYETOY)
		{
			memcpy(s->regs, ov519_defaults, sizeof(s->regs));
		}
		else if (s->subtype == TYPE_OV511P)
		{
			memcpy(s->regs, ov511p_defaults, sizeof(s->regs));
		}
	}

	static void reset_sensor(EYETOYState* s)
	{
		if (s->subtype == TYPE_EYETOY)
		{
			memcpy(s->i2c_regs, ov7648_defaults, sizeof(s->regs));
		}
		else if (s->subtype == TYPE_OV511P)
		{
			memcpy(s->i2c_regs, ov7620_defaults, sizeof(s->regs));
		}
	}

	static void open_camera(EYETOYState* s)
	{
		if (s->hw_camera_running && s->subtype == TYPE_EYETOY)
		{
			// The game writes the size; the frame buffers hold 640x480.
			const int width = std::clamp(s->regs[OV519_R10_H_SIZE] << 4, 16, 640);
			const int height = std::clamp(s->regs[OV519_R11_V_SIZE] << 3, 8, 480);
			const FrameFormat format = s->regs[OV519_RA0_FORMAT] == OV519_RA0_FORMAT_JPEG ? format_jpeg : format_mpeg;
			const int mirror = !!(s->i2c_regs[OV7610_REG_COM_A] & OV7610_REG_COM_A_MASK_MIRROR);
			log_cb(RETRO_LOG_INFO,
				"EyeToy : eyetoy_open(); hw=%d, w=%d, h=%d, fmt=%d, mirr=%d\n", s->hw_camera_running, width, height, format, mirror);
			if (s->videodev->Open(width, height, format, mirror) != 0)
				log_cb(RETRO_LOG_ERROR, "(Eyetoy) Failed to open video device\n");
		}
		else if (s->hw_camera_running && s->subtype == TYPE_OV511P)
		{
			constexpr int width = 320;
			constexpr int height = 240;
			const FrameFormat format = format_yuv400;
			constexpr int mirror = 0;
			log_cb(RETRO_LOG_INFO,
				"EyeToy : eyetoy_open(); hw=%d, w=%d, h=%d, fmt=%d, mirr=%d\n", s->hw_camera_running, width, height, format, mirror);
			if (s->videodev->Open(width, height, format, mirror) != 0)
				log_cb(RETRO_LOG_ERROR, "(Eyetoy) Failed to open video device\n");
		}
	}

	static void close_camera(EYETOYState* s)
	{
		log_cb(RETRO_LOG_INFO, "EyeToy : eyetoy_close(); hw=%d\n", s->hw_camera_running);
		if (s->hw_camera_running)
		{
			s->hw_camera_running = 0;
			s->videodev->Close();
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
		int ret = 0;

		ret = usb_desc_handle_control(dev, p, request, value, index, length, data);
		if (ret >= 0)
		{
			return;
		}

		switch (request)
		{
			case VendorDeviceRequest | 0x1: //Read register
				data[0] = s->regs[index & 0xFF];
				p->actual_length = 1;
				break;

			case VendorDeviceOutRequest | 0x1: //Write register
				switch (index)
				{
					case OV519_RA0_FORMAT:
						if (data[0] == OV519_RA0_FORMAT_MPEG)
						{
							log_cb(RETRO_LOG_INFO, "EyeToy : configured for MPEG format\n");
						}
						else if (data[0] == OV519_RA0_FORMAT_JPEG)
						{
							log_cb(RETRO_LOG_INFO, "EyeToy : configured for JPEG format\n");
						}
						else
						{
							log_cb(RETRO_LOG_INFO, "EyeToy : configured for unknown format\n");
						}

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
						static char led_state = -1;
						if (led_state != data[0])
						{
							led_state = data[0];
							log_cb(RETRO_LOG_INFO, "EyeToy : LED : %d\n", !!led_state);
						}
					}
					break;
					case R518_I2C_CTL:
						if (data[0] == 1) // Commit I2C write
						{
							//u8 reg = s->regs[s->regs[R51x_I2C_W_SID]];
							const u8 reg = s->regs[R51x_I2C_SADDR_3];
							const u8 val = s->regs[R51x_I2C_DATA];
							if ((reg == 0x12) && (val & 0x80))
							{
								s->i2c_regs[0x12] = val & ~0x80; //or skip?
								reset_sensor(s);
							}
							else if (reg < sizeof(s->i2c_regs))
							{
								s->i2c_regs[reg] = val;
							}
							if (reg == OV7610_REG_COM_A)
							{
								const bool mirroring_enabled = val & OV7610_REG_COM_A_MASK_MIRROR;
								s->videodev->SetMirroring(mirroring_enabled);
								log_cb(RETRO_LOG_INFO, "EyeToy : mirroring %s\n", mirroring_enabled ? "ON" : "OFF");
							}
						}
						else if (s->regs[R518_I2C_CTL] == 0x03 && data[0] == 0x05)
						{
							//s->regs[s->regs[R51x_I2C_R_SID]] but seems to default to 0x43 (R51x_I2C_SADDR_2)
							const u8 i2c_reg = s->regs[R51x_I2C_SADDR_2];
							s->regs[R51x_I2C_DATA] = 0;

							if (i2c_reg < sizeof(s->i2c_regs))
							{
								s->regs[R51x_I2C_DATA] = s->i2c_regs[i2c_reg];
							}
						}
						break;
					default:
						break;
				}

				//Max 0xFFFF regs?
				s->regs[index & 0xFF] = data[0];
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
		int ret = 0;

		ret = usb_desc_handle_control(dev, p, request, value, index, length, data);
		if (ret >= 0)
		{
			return;
		}

		switch (request)
		{
			case VendorDeviceRequest | 0x3: //Read register
				data[0] = s->regs[index & 0xFF];
				p->actual_length = 1;
				break;

			case VendorDeviceOutRequest | 0x2: //Write register
				switch (index)
				{
					case R511_I2C_CTL:
						if (data[0] == 1)
						{
							u8 reg = s->regs[R51x_I2C_SADDR_3];
							const u8 val = s->regs[R51x_I2C_DATA];
							if (reg < sizeof(s->i2c_regs))
							{
								s->i2c_regs[reg] = val;
							}
						}
						else if (s->regs[R511_I2C_CTL] == 0x03 && data[0] == 0x05)
						{
							const u8 i2c_reg = s->regs[R51x_I2C_SADDR_2];
							s->regs[R51x_I2C_DATA] = 0;

							if (i2c_reg < sizeof(s->i2c_regs))
							{
								s->regs[R51x_I2C_DATA] = s->i2c_regs[i2c_reg];
							}
						}
						break;
					default:
						break;
				}

				s->regs[index & 0xFF] = data[0];
				p->actual_length = 1;

				break;
			default:
				p->status = USB_RET_STALL;
				break;
		}
	}

	static void webcam_handle_data_eyetoy(USBDevice* dev, USBPacket* p)
	{
		EYETOYState* s = USB_CONTAINER_OF(dev, EYETOYState, dev);
		static constexpr unsigned int max_ep_size = 896;
		const u8 devep = p->ep->nr;

		if (!s->hw_camera_running)
		{
			log_cb(RETRO_LOG_INFO, "EyeToy : initialization done; start the camera\n");
			s->hw_camera_running = 1;
			open_camera(s);
		}

		switch (p->pid)
		{
			case USB_TOKEN_IN:
				u8 data[max_ep_size];
				if (devep == 1)
				{
					if (s->frame_step == 0)
					{
						s->mpeg_frame_size = s->videodev->GetImage(s->mpeg_frame_data.get(), 640 * 480 * 3);
						if (s->mpeg_frame_size == 0)
						{
							p->status = USB_RET_NAK;
							break;
						}

						u8 header[] = {0xFF, 0xFF, 0xFF, 0x50, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
						header[0x0A] = s->regs[OV519_RA0_FORMAT] == OV519_RA0_FORMAT_JPEG ? 0x03 : 0x01;
						std::memcpy(data, header, sizeof(header));

						const u32 data_pk = std::min(std::min(p->buffer_size, max_ep_size) - static_cast<u32>(sizeof(header)), s->mpeg_frame_size);
						std::memcpy(data + sizeof(header), s->mpeg_frame_data.get(), data_pk);
						usb_packet_copy(p, data, sizeof(header) + data_pk);

						s->mpeg_frame_offset = data_pk;
						s->frame_step++;
					}
					else if (s->mpeg_frame_offset < s->mpeg_frame_size)
					{
						const u32 data_pk = std::min(s->mpeg_frame_size - s->mpeg_frame_offset, p->buffer_size);
						usb_packet_copy(p, s->mpeg_frame_data.get() + s->mpeg_frame_offset, data_pk);

						s->mpeg_frame_offset += data_pk;
						s->frame_step++;
					}
					else
					{
						u8 footer[] = {0xFF, 0xFF, 0xFF, 0x51, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
						footer[0x0A] = s->regs[OV519_RA0_FORMAT] == OV519_RA0_FORMAT_JPEG ? 0x03 : 0x01;
						usb_packet_copy(p, footer, sizeof(footer));
						s->frame_step = 0;
					}
				}
				else if (devep == 2)
				{
					// One millisecond of 16 kHz mono per packet.
					s16 samples[16];
					const u32 n = s->mic ? s->mic->Read(samples, std::min<u32>(p->buffer_size / 2, 16)) : 0;
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
		EYETOYState* s = USB_CONTAINER_OF(dev, EYETOYState, dev);
		const u8 devep = p->ep->nr;

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
						s->mpeg_frame_size = s->videodev->GetImage(s->mpeg_frame_data.get(), 640 * 480 * 3);
						if (s->mpeg_frame_size == 0)
						{
							p->status = USB_RET_NAK;
							break;
						}

						static const unsigned int max_ep_size = 961;
						u8 data[max_ep_size];

						static constexpr const u8 header[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x28}; // 28 <> 29
						std::memcpy(data, header, sizeof(header));

						const u32 data_pk = std::min(std::min(p->buffer_size, max_ep_size) - 1 - static_cast<u32>(sizeof(header)), s->mpeg_frame_size);
						std::memcpy(data + sizeof(header), s->mpeg_frame_data.get(), data_pk);
						usb_packet_copy(p, data, sizeof(header) + data_pk);

						s->mpeg_frame_offset = data_pk;
						s->frame_step++;
					}
					else if (s->mpeg_frame_offset < s->mpeg_frame_size)
					{
						const u32 data_pk = std::min(s->mpeg_frame_size - s->mpeg_frame_offset, p->buffer_size - 1);
						usb_packet_copy(p, s->mpeg_frame_data.get() + s->mpeg_frame_offset, data_pk);

						s->mpeg_frame_offset += data_pk;
						s->frame_step++;
					}
					else
					{
						static constexpr const u8 footer[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xA8, 0x09, 0x07};
						usb_packet_copy(p, const_cast<u8*>(footer), sizeof(footer));
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
		delete s;
	}

	static void eyetoy_freeze(USBDevice* dev, UsbState* st)
	{
		EYETOYState* s = USB_CONTAINER_OF(dev, EYETOYState, dev);
		usb_state_bytes(st, s->regs, sizeof(s->regs));
		usb_state_bytes(st, s->i2c_regs, sizeof(s->i2c_regs));
		if (st->reading)
		{
			// The frame in flight is not kept: the camera starts afresh on the next request,
			// with the format and size the registers now hold.
			close_camera(s);
			s->frame_step = 0;
			s->mpeg_frame_size = 0;
			s->mpeg_frame_offset = 0;
		}
	}

	static USBDevice* eyetoy_create(u32 subtype)
	{
		std::unique_ptr<VideoDevice> videodev(VideoDevice::CreateInstance());
		if (!videodev)
		{
			log_cb(RETRO_LOG_ERROR, "EyeToy: failed to create video device\n");
			return nullptr;
		}

		EYETOYState* s = new EYETOYState();
		s->subtype = subtype;
		s->desc.full = &s->desc_dev;
		s->desc.str = desc_strings;

		if (subtype == TYPE_EYETOY)
		{
			if (usb_desc_parse_dev(eyetoy_dev_descriptor, sizeof(eyetoy_dev_descriptor), &s->desc, &s->desc_dev) < 0)
				goto fail;
			if (usb_desc_parse_config(eyetoy_config_descriptor, sizeof(eyetoy_config_descriptor), &s->desc_dev) < 0)
				goto fail;
			s->dev.klass.handle_control = webcam_handle_control_eyetoy;
			s->dev.klass.handle_data = webcam_handle_data_eyetoy;

			s->mic = MicSource::CreateInstance();
		}
		else if (subtype == TYPE_OV511P)
		{
			if (usb_desc_parse_dev(ov511p_dev_descriptor, sizeof(ov511p_dev_descriptor), &s->desc, &s->desc_dev) < 0)
				goto fail;
			if (usb_desc_parse_config(ov511p_config_descriptor, sizeof(ov511p_config_descriptor), &s->desc_dev) < 0)
				goto fail;
			s->dev.klass.handle_control = webcam_handle_control_ov511p;
			s->dev.klass.handle_data = webcam_handle_data_ov511p;
		}

		s->videodev = std::move(videodev);
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
		s->mpeg_frame_data = std::make_unique<unsigned char[]>(640 * 480 * 3);
		std::memset(s->mpeg_frame_data.get(), 0, 640 * 480 * 3);
		s->mpeg_frame_offset = 0;

		return &s->dev;
	fail:
		eyetoy_handle_destroy(&s->dev);
		return nullptr;
	}
} // namespace usb_eyetoy

USBDevice* usb_eyetoy_create(int model)
{
	return usb_eyetoy::eyetoy_create(model == USB_EYETOY_MODEL_EYETOY ? usb_eyetoy::TYPE_EYETOY : usb_eyetoy::TYPE_OV511P);
}

void usb_eyetoy_freeze(USBDevice* dev, UsbState* st)
{
	usb_eyetoy::eyetoy_freeze(dev, st);
}

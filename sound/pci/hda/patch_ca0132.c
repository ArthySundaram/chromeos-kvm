/*
 * HD audio interface patch for Creative Sound Core3D chip
 *
 * Copyright (c) 2011, Creative Technology Ltd.
 *
 * Based on patch_ca0110.c
 * Copyright (c) 2008 Takashi Iwai <tiwai@suse.de>
 *
 *  This driver is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This driver is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 */

#include <linux/init.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/pci.h>
#include <linux/module.h>
#include <linux/firmware.h>
#include <sound/core.h>
#include <sound/tlv.h>
#include "hda_codec.h"
#include "hda_local.h"
#include "hda_jack.h"

#include "ca0132_regs.h"

/* Enable this to see more debug messages. */
/*#define ENABLE_CA0132_DEBUG*/
/* Enable this to see controls for tuning purpose. */
/*#define ENABLE_TUNING_CONTROLS*/

#define FLOAT_ZERO	0x00000000
#define FLOAT_ONE	0x3f800000
#define FLOAT_TWO	0x40000000
#define FLOAT_MINUS_5	0xc0a00000
#define FLOAT_60	0x42700000;

#define UNSOL_TAG_HP	0x10
#define UNSOL_TAG_AMIC1	0x12
#define UNSOL_TAG_DSP	0x16

#define SUCCEEDED(_x)         (((int)(_x)) >= 0)
#define FAILED(_x)            (((int)(_x)) < 0)

#define DSP_DMA_WRITE_BUFLEN_INIT (1UL<<18)
#define DSP_DMA_WRITE_BUFLEN_OVLY (1UL<<15)

#define DMA_TRANSFER_FRAME_SIZE_NWORDS		8
#define DMA_TRANSFER_MAX_FRAME_SIZE_NWORDS	32
#define DMA_OVERLAY_FRAME_SIZE_NWORDS		2

#define MASTERCONTROL				0x80
#define MASTERCONTROL_ALLOC_DMA_CHAN		10
#define MASTERCONTROL_QUERY_SPEAKER_EQ_ADDRESS	60

#define WIDGET_CHIP_CTRL      0x15
#define WIDGET_DSP_CTRL       0x16

#define MEM_CONNID_MICIN1     3
#define MEM_CONNID_MICIN2     5
#define MEM_CONNID_MICOUT1    12
#define MEM_CONNID_MICOUT2    14
#define MEM_CONNID_WUH        10
#define MEM_CONNID_DSP        16
#define MEM_CONNID_DMIC       100

#define SCP_SET    0
#define SCP_GET    1

#define SPEQ_FILE  "ctspeq.bin"
#define EFX_FILE   "ctefx.bin"

MODULE_FIRMWARE(SPEQ_FILE);
MODULE_FIRMWARE(EFX_FILE);

/* Debug message controls */
#ifdef ENABLE_CA0132_DEBUG

#define CTASSERT(x) \
	{ if (!(x)) snd_printdd(KERN_ERR "CTASSERT failed.\n"); }

#define CA0132_LOG(fmt, args...)    snd_printdd(fmt, ##args)
#define CA0132_DSP_LOG(msg)         snd_printdd(KERN_INFO "[%s]\n", msg)
#define FAIL_MSG(n, s)              fail_debug_out(n, s)

static int fail_debug_out(int status, const char *s)
{
	snd_printdd(KERN_ERR "[%s]\n", s);
	return status;
}
#else

#define CTASSERT(x)                 do { } while (0)
#define CA0132_LOG(fmt, args...)    do { } while (0)
#define CA0132_DSP_LOG(msg)         do { } while (0)
#define FAIL_MSG(n, s)              fail_debug_out(n, s)

static int fail_debug_out(int status, const char *s)
{
	return status;
}
#endif

static char *dirstr[2] = { "Playback", "Capture" };

enum {
	SPEAKER_OUT,
	HEADPHONE_OUT
};

enum {
	DIGITAL_MIC,
	LINE_MIC_IN
};

enum {
#define VNODE_START_NID    0x80
	VNID_SPK = VNODE_START_NID,			/* Speaker vnid */
	VNID_MIC,
	VNID_HP_SEL,
	VNID_AMIC1_SEL,
	VNID_HP_ASEL,
	VNID_AMIC1_ASEL,
	VNODE_END_NID,
#define VNODES_COUNT  (VNODE_END_NID - VNODE_START_NID)

#define EFFECT_START_NID    0x90
#define OUT_EFFECT_START_NID    EFFECT_START_NID
	SURROUND = OUT_EFFECT_START_NID,
	CRYSTALIZER,
	DIALOG_PLUS,
	SMART_VOLUME,
	X_BASS,
	EQUALIZER,
	OUT_EFFECT_END_NID,
#define OUT_EFFECTS_COUNT  (OUT_EFFECT_END_NID - OUT_EFFECT_START_NID)

#define IN_EFFECT_START_NID  OUT_EFFECT_END_NID
	ECHO_CANCELLATION = IN_EFFECT_START_NID,
	VOICE_FOCUS,
	MIC_SVM,
	NOISE_REDUCTION,
	IN_EFFECT_END_NID,
#define IN_EFFECTS_COUNT  (IN_EFFECT_END_NID - IN_EFFECT_START_NID)

	VOICEFX = IN_EFFECT_END_NID,
	PLAY_ENHANCEMENT,
	CRYSTAL_VOICE,
	EFFECT_END_NID
#define EFFECTS_COUNT  (EFFECT_END_NID - EFFECT_START_NID)
};

/* Effects values size*/
#define EFFECT_VALS_MAX_COUNT 12

#define DSP_CAPTURE_INIT_LATENCY        0
#define DSP_CRYSTAL_VOICE_LATENCY       124
#define DSP_PLAYBACK_INIT_LATENCY       13
#define DSP_PLAY_ENHANCEMENT_LATENCY    30
#define DSP_SPEAKER_OUT_LATENCY         7

struct ct_effect {
	char name[44];
	hda_nid_t nid;
	int mid; /*effect module ID*/
	int reqs[EFFECT_VALS_MAX_COUNT]; /*effect module request*/
	int direct; /* 0:output; 1:input*/
	int params; /* number of default non-on/off params */
	/*effect default values, 1st is on/off. */
	unsigned int def_vals[EFFECT_VALS_MAX_COUNT];
};

static struct ct_effect ca0132_effects[EFFECTS_COUNT] = {
	{ "Surround",
		SURROUND,
		0x96,
		{0, 1},
		0,
		1,
		{0x3F800000, 0x3F2B851F}
	},
	{ "Crystalizer",
		CRYSTALIZER,
		0x96,
		{7, 8},
		0,
		1,
		{0x3F800000, 0x3F266666}
	},
	{ "Dialog Plus",
		DIALOG_PLUS,
		0x96,
		{2, 3},
		0,
		1,
		{0x00000000, 0x3F000000}
	},
	{ "Smart Volume",
		SMART_VOLUME,
		0x96,
		{4, 5, 6},
		0,
		2,
		{0x3F800000, 0x3F3D70A4, 0x00000000}
	},
	{ "X-Bass",
		X_BASS,
		0x96,
		{24, 23, 25},
		0,
		2,
		{0x3F800000, 0x42A00000, 0x3F000000}
	},
	{ "Equalizer",
		EQUALIZER,
		0x96,
		{9, 10, 11, 12, 13, 14,
			15, 16, 17, 18, 19, 20},
		0,
		11,
		{0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
		 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
		 0x00000000, 0x00000000}
	},
	{ "Echo Cancellation",
		ECHO_CANCELLATION,
		0x95,
		{0, 1, 2, 3},
		1,
		3,
		{0x00000000, 0x3F3A9692, 0x00000000, 0x00000000}
	},
	{ "Voice Focus",
		VOICE_FOCUS,
		0x95,
		{6, 7, 8, 9},
		1,
		3,
		{0x3F800000, 0x3D7DF3B6, 0x41F00000, 0x41F00000}
	},
	{ "Mic SVM",
		MIC_SVM,
		0x95,
		{44, 45},
		1,
		1,
		{0x00000000, 0x3F3D70A4}
	},
	{ "Noise Reduction",
		NOISE_REDUCTION,
		0x95,
		{4, 5},
		1,
		1,
		{0x3F800000, 0x3F000000}
	},
	{ "VoiceFX",
		VOICEFX,
		0x95,
		{10, 11, 12, 13, 14, 15, 16, 17, 18},
		1,
		8,
		{0x00000000, 0x43C80000, 0x44AF0000, 0x44FA0000, 0x3F800000,
		 0x3F800000, 0x3F800000, 0x00000000, 0x00000000}
	}
};


/* Tuning controls */
#ifdef ENABLE_TUNING_CONTROLS

enum {
#define TUNING_CTL_START_NID  0xC0
	WEDGE_ANGLE = TUNING_CTL_START_NID,
	SVM_LEVEL,
	EQUALIZER_BAND_0,
	EQUALIZER_BAND_1,
	EQUALIZER_BAND_2,
	EQUALIZER_BAND_3,
	EQUALIZER_BAND_4,
	EQUALIZER_BAND_5,
	EQUALIZER_BAND_6,
	EQUALIZER_BAND_7,
	EQUALIZER_BAND_8,
	EQUALIZER_BAND_9,
	TUNING_CTL_END_NID
#define TUNING_CTLS_COUNT  (TUNING_CTL_END_NID - TUNING_CTL_START_NID)
};

struct ct_tuning_ctl {
	char name[44];
	hda_nid_t parent_nid;
	hda_nid_t nid;
	int mid; /*effect module ID*/
	int req; /*effect module request*/
	int direct; /* 0:output; 1:input*/
	unsigned int def_val;/*effect default values*/
};

static struct ct_tuning_ctl ca0132_tuning_ctls[] = {
	{ "Wedge Angle",
		VOICE_FOCUS,
		WEDGE_ANGLE,
		0x95,
		8,
		1,
		0x41F00000
	},
	{ "SVM Level",
		MIC_SVM,
		SVM_LEVEL,
		0x95,
		45,
		1,
		0x3F3D70A4
	},
	{ "EQ Band0",
		EQUALIZER,
		EQUALIZER_BAND_0,
		0x96,
		11,
		0,
		0x00000000
	},
	{ "EQ Band1",
		EQUALIZER,
		EQUALIZER_BAND_1,
		0x96,
		12,
		0,
		0x00000000
	},
	{ "EQ Band2",
		EQUALIZER,
		EQUALIZER_BAND_2,
		0x96,
		13,
		0,
		0x00000000
	},
	{ "EQ Band3",
		EQUALIZER,
		EQUALIZER_BAND_3,
		0x96,
		14,
		0,
		0x00000000
	},
	{ "EQ Band4",
		EQUALIZER,
		EQUALIZER_BAND_4,
		0x96,
		15,
		0,
		0x00000000
	},
	{ "EQ Band5",
		EQUALIZER,
		EQUALIZER_BAND_5,
		0x96,
		16,
		0,
		0x00000000
	},
	{ "EQ Band6",
		EQUALIZER,
		EQUALIZER_BAND_6,
		0x96,
		17,
		0,
		0x00000000
	},
	{ "EQ Band7",
		EQUALIZER,
		EQUALIZER_BAND_7,
		0x96,
		18,
		0,
		0x00000000
	},
	{ "EQ Band8",
		EQUALIZER,
		EQUALIZER_BAND_8,
		0x96,
		19,
		0,
		0x00000000
	},
	{ "EQ Band9",
		EQUALIZER,
		EQUALIZER_BAND_9,
		0x96,
		20,
		0,
		0x00000000
	}
};
#endif

/* Voice FX Presets */
#define VOICEFX_MAX_PARAM_COUNT 9

struct ct_voicefx {
	char *name;
	hda_nid_t nid;
	int mid;
	int reqs[VOICEFX_MAX_PARAM_COUNT]; /*effect module request*/
};

struct ct_voicefx_preset {
	char *name; /*preset name*/
	unsigned int vals[VOICEFX_MAX_PARAM_COUNT];
};

struct ct_voicefx ca0132_voicefx = {
	"VoiceFX Capture Switch",
	VOICEFX,
	0x95,
	{10, 11, 12, 13, 14, 15, 16, 17, 18}
};

struct ct_voicefx_preset ca0132_voicefx_presets[] = {
	{ "Neutral",
		{ 0x00000000, 0x43C80000, 0x44AF0000,
		  0x44FA0000, 0x3F800000, 0x3F800000,
		  0x3F800000, 0x00000000, 0x00000000 }
	},
	{ "Female2Male",
		{ 0x3F800000, 0x43C80000, 0x44AF0000,
		  0x44FA0000, 0x3F19999A, 0x3F866666,
		  0x3F800000, 0x00000000, 0x00000000 }
	},
	{ "Male2Female",
		{ 0x3F800000, 0x43C80000, 0x44AF0000,
		  0x450AC000, 0x4017AE14, 0x3F6B851F,
		  0x3F800000, 0x00000000, 0x00000000 }
	},
	{ "ScrappyKid",
		{ 0x3F800000, 0x43C80000, 0x44AF0000,
		  0x44FA0000, 0x40400000, 0x3F28F5C3,
		  0x3F800000, 0x00000000, 0x00000000 }
	},
	{ "Elderly",
		{ 0x3F800000, 0x44324000, 0x44BB8000,
		  0x44E10000, 0x3FB33333, 0x3FB9999A,
		  0x3F800000, 0x3E3A2E43, 0x00000000 }
	},
	{ "Orc",
		{ 0x3F800000, 0x43EA0000, 0x44A52000,
		  0x45098000, 0x3F266666, 0x3FC00000,
		  0x3F800000, 0x00000000, 0x00000000 }
	},
	{ "Elf",
		{ 0x3F800000, 0x43C70000, 0x44AE6000,
		  0x45193000, 0x3F8E147B, 0x3F75C28F,
		  0x3F800000, 0x00000000, 0x00000000 }
	},
	{ "Dwarf",
		{ 0x3F800000, 0x43930000, 0x44BEE000,
		  0x45007000, 0x3F451EB8, 0x3F7851EC,
		  0x3F800000, 0x00000000, 0x00000000 }
	},
	{ "AlienBrute",
		{ 0x3F800000, 0x43BFC5AC, 0x44B28FDF,
		  0x451F6000, 0x3F266666, 0x3FA7D945,
		  0x3F800000, 0x3CF5C28F, 0x00000000 }
	},
	{ "Robot",
		{ 0x3F800000, 0x43C80000, 0x44AF0000,
		  0x44FA0000, 0x3FB2718B, 0x3F800000,
		  0xBC07010E, 0x00000000, 0x00000000 }
	},
	{ "Marine",
		{ 0x3F800000, 0x43C20000, 0x44906000,
		  0x44E70000, 0x3F4CCCCD, 0x3F8A3D71,
		  0x3F0A3D71, 0x00000000, 0x00000000 }
	},
	{ "Emo",
		{ 0x3F800000, 0x43C80000, 0x44AF0000,
		  0x44FA0000, 0x3F800000, 0x3F800000,
		  0x3E4CCCCD, 0x00000000, 0x00000000 }
	},
	{ "DeepVoice",
		{ 0x3F800000, 0x43A9C5AC, 0x44AA4FDF,
		  0x44FFC000, 0x3EDBB56F, 0x3F99C4CA,
		  0x3F800000, 0x00000000, 0x00000000 }
	},
	{ "Munchkin",
		{ 0x3F800000, 0x43C80000, 0x44AF0000,
		  0x44FA0000, 0x3F800000, 0x3F1A043C,
		  0x3F800000, 0x00000000, 0x00000000 }
	}
};

enum hda_cmd_vendor_io {
	/* for DspIO node */
	VENDOR_DSPIO_SCP_WRITE_DATA_LOW      = 0x000,
	VENDOR_DSPIO_SCP_WRITE_DATA_HIGH     = 0x100,

	VENDOR_DSPIO_STATUS                  = 0xF01,
	VENDOR_DSPIO_SCP_POST_READ_DATA      = 0x702,
	VENDOR_DSPIO_SCP_READ_DATA           = 0xF02,
	VENDOR_DSPIO_DSP_INIT                = 0x703,
	VENDOR_DSPIO_SCP_POST_COUNT_QUERY    = 0x704,
	VENDOR_DSPIO_SCP_READ_COUNT          = 0xF04,

	/* for ChipIO node */
	VENDOR_CHIPIO_ADDRESS_LOW            = 0x000,
	VENDOR_CHIPIO_ADDRESS_HIGH           = 0x100,
	VENDOR_CHIPIO_STREAM_FORMAT          = 0x200,
	VENDOR_CHIPIO_DATA_LOW               = 0x300,
	VENDOR_CHIPIO_DATA_HIGH              = 0x400,

	VENDOR_CHIPIO_GET_PARAMETER          = 0xF00,
	VENDOR_CHIPIO_STATUS                 = 0xF01,
	VENDOR_CHIPIO_HIC_POST_READ          = 0x702,
	VENDOR_CHIPIO_HIC_READ_DATA          = 0xF03,

	VENDOR_CHIPIO_8051_DATA_WRITE        = 0x707,
	VENDOR_CHIPIO_8051_DATA_READ         = 0xF07,

	VENDOR_CHIPIO_CT_EXTENSIONS_ENABLE   = 0x70A,
	VENDOR_CHIPIO_CT_EXTENSIONS_GET      = 0xF0A,

	VENDOR_CHIPIO_PLL_PMU_WRITE          = 0x70C,
	VENDOR_CHIPIO_PLL_PMU_READ           = 0xF0C,
	VENDOR_CHIPIO_8051_ADDRESS_LOW       = 0x70D,
	VENDOR_CHIPIO_8051_ADDRESS_HIGH      = 0x70E,
	VENDOR_CHIPIO_FLAG_SET               = 0x70F,
	VENDOR_CHIPIO_FLAGS_GET              = 0xF0F,
	VENDOR_CHIPIO_PARAM_SET              = 0x710,
	VENDOR_CHIPIO_PARAM_GET              = 0xF10,

	VENDOR_CHIPIO_PORT_ALLOC_CONFIG_SET  = 0x711,
	VENDOR_CHIPIO_PORT_ALLOC_SET         = 0x712,
	VENDOR_CHIPIO_PORT_ALLOC_GET         = 0xF12,
	VENDOR_CHIPIO_PORT_FREE_SET          = 0x713,

	VENDOR_CHIPIO_PARAM_EX_ID_GET        = 0xF17,
	VENDOR_CHIPIO_PARAM_EX_ID_SET        = 0x717,
	VENDOR_CHIPIO_PARAM_EX_VALUE_GET     = 0xF18,
	VENDOR_CHIPIO_PARAM_EX_VALUE_SET     = 0x718,

	VENDOR_CHIPIO_DMIC_CTL_SET           = 0x788,
	VENDOR_CHIPIO_DMIC_CTL_GET           = 0xF88,
	VENDOR_CHIPIO_DMIC_PIN_SET           = 0x789,
	VENDOR_CHIPIO_DMIC_PIN_GET           = 0xF89,
	VENDOR_CHIPIO_DMIC_MCLK_SET          = 0x78A,
	VENDOR_CHIPIO_DMIC_MCLK_GET          = 0xF8A,

	VENDOR_CHIPIO_EAPD_SEL_SET           = 0x78D
};

enum control_flag_id {
	/* Connection manager stream setup is bypassed/enabled */
	CONTROL_FLAG_C_MGR                  = 0,
	/* DSP DMA is bypassed/enabled */
	CONTROL_FLAG_DMA                    = 1,
	/* 8051 'idle' mode is disabled/enabled */
	CONTROL_FLAG_IDLE_ENABLE            = 2,
	/* Tracker for the SPDIF-in path is bypassed/enabled */
	CONTROL_FLAG_TRACKER                = 3,
	/* DigitalOut to Spdif2Out connection is disabled/enabled */
	CONTROL_FLAG_SPDIF2OUT              = 4,
	/* Digital Microphone is disabled/enabled */
	CONTROL_FLAG_DMIC                   = 5,
	/* ADC_B rate is 48 kHz/96 kHz */
	CONTROL_FLAG_ADC_B_96KHZ            = 6,
	/* ADC_C rate is 48 kHz/96 kHz */
	CONTROL_FLAG_ADC_C_96KHZ            = 7,
	/* DAC rate is 48 kHz/96 kHz (affects all DACs) */
	CONTROL_FLAG_DAC_96KHZ              = 8,
	/* DSP rate is 48 kHz/96 kHz */
	CONTROL_FLAG_DSP_96KHZ              = 9,
	/* SRC clock is 98 MHz/196 MHz (196 MHz forces rate to 96 KHz) */
	CONTROL_FLAG_SRC_CLOCK_196MHZ       = 10,
	/* SRC rate is 48 kHz/96 kHz (48 kHz disabled when clock is 196 MHz) */
	CONTROL_FLAG_SRC_RATE_96KHZ         = 11,
	/* Decode Loop (DSP->SRC->DSP) is disabled/enabled */
	CONTROL_FLAG_DECODE_LOOP            = 12,
	/* De-emphasis filter on DAC-1 disabled/enabled */
	CONTROL_FLAG_DAC1_DEEMPHASIS        = 13,
	/* De-emphasis filter on DAC-2 disabled/enabled */
	CONTROL_FLAG_DAC2_DEEMPHASIS        = 14,
	/* De-emphasis filter on DAC-3 disabled/enabled */
	CONTROL_FLAG_DAC3_DEEMPHASIS        = 15,
	/* High-pass filter on ADC_B disabled/enabled */
	CONTROL_FLAG_ADC_B_HIGH_PASS        = 16,
	/* High-pass filter on ADC_C disabled/enabled */
	CONTROL_FLAG_ADC_C_HIGH_PASS        = 17,
	/* Common mode on Port_A disabled/enabled */
	CONTROL_FLAG_PORT_A_COMMON_MODE     = 18,
	/* Common mode on Port_D disabled/enabled */
	CONTROL_FLAG_PORT_D_COMMON_MODE     = 19,
	/* Impedance for ramp generator on Port_A 16 Ohm/10K Ohm */
	CONTROL_FLAG_PORT_A_10KOHM_LOAD     = 20,
	/* Impedance for ramp generator on Port_D, 16 Ohm/10K Ohm */
	CONTROL_FLAG_PORT_D_10KOHM_LOAD     = 21,
	/* ASI rate is 48kHz/96kHz */
	CONTROL_FLAG_ASI_96KHZ              = 22,
	/* DAC power settings able to control attached ports no/yes */
	CONTROL_FLAG_DACS_CONTROL_PORTS     = 23,
	/* Clock Stop OK reporting is disabled/enabled */
	CONTROL_FLAG_CONTROL_STOP_OK_ENABLE = 24,
	/* Number of control flags */
	CONTROL_FLAGS_MAX = (CONTROL_FLAG_CONTROL_STOP_OK_ENABLE+1)
};

enum control_param_id {
	/* 0: None, 1: Mic1In*/
	CONTROL_PARAM_VIP_SOURCE               = 1,
	/* 0: force HDA, 1: allow DSP if HDA Spdif1Out stream is idle */
	CONTROL_PARAM_SPDIF1_SOURCE            = 2,
	/* Port A output stage gain setting to use when 16 Ohm output
	 * impedance is selected*/
	CONTROL_PARAM_PORTA_160OHM_GAIN        = 8,
	/* Port D output stage gain setting to use when 16 Ohm output
	 * impedance is selected*/
	CONTROL_PARAM_PORTD_160OHM_GAIN        = 10,

	/* Stream Control */

	/* Select stream with the given ID */
	CONTROL_PARAM_STREAM_ID                = 24,
	/* Source connection point for the selected stream */
	CONTROL_PARAM_STREAM_SOURCE_CONN_POINT = 25,
	/* Destination connection point for the selected stream */
	CONTROL_PARAM_STREAM_DEST_CONN_POINT   = 26,
	/* Number of audio channels in the selected stream */
	CONTROL_PARAM_STREAMS_CHANNELS         = 27,
	/*Enable control for the selected stream */
	CONTROL_PARAM_STREAM_CONTROL           = 28,

	/* Connection Point Control */

	/* Select connection point with the given ID */
	CONTROL_PARAM_CONN_POINT_ID            = 29,
	/* Connection point sample rate */
	CONTROL_PARAM_CONN_POINT_SAMPLE_RATE   = 30,

	/* Node Control */

	/* Select HDA node with the given ID */
	CONTROL_PARAM_NODE_ID                  = 31
};

enum hda_vendor_status_dspio {
	/* Success */
	VENDOR_STATUS_DSPIO_OK                       = 0x00,
	/* Busy, unable to accept new command, the host must retry */
	VENDOR_STATUS_DSPIO_BUSY                     = 0x01,
	/* SCP command queue is full */
	VENDOR_STATUS_DSPIO_SCP_COMMAND_QUEUE_FULL   = 0x02,
	/* SCP response queue is empty */
	VENDOR_STATUS_DSPIO_SCP_RESPONSE_QUEUE_EMPTY = 0x03
};

enum hda_vendor_status_chipio {
	/* Success */
	VENDOR_STATUS_CHIPIO_OK   = 0x00,
	/* Busy, unable to accept new command, the host must retry */
	VENDOR_STATUS_CHIPIO_BUSY = 0x01
};

enum ca0132_sample_rate {
	SR_6_000        = 0x00,
	SR_8_000        = 0x01,
	SR_9_600        = 0x02,
	SR_11_025       = 0x03,
	SR_16_000       = 0x04,
	SR_22_050       = 0x05,
	SR_24_000       = 0x06,
	SR_32_000       = 0x07,
	SR_44_100       = 0x08,
	SR_48_000       = 0x09,
	SR_88_200       = 0x0A,
	SR_96_000       = 0x0B,
	SR_144_000      = 0x0C,
	SR_176_400      = 0x0D,
	SR_192_000      = 0x0E,
	SR_384_000      = 0x0F,

	SR_COUNT        = 0x10,

	SR_RATE_UNKNOWN = 0x1F
};

enum dsp_download_state {
	DSP_DOWNLOAD_FAILED = -1,
	DSP_DOWNLOAD_INIT   = 0,
	DSP_DOWNLOADING     = 1,
	DSP_DOWNLOADED      = 2
};

struct hda_stream_format {
	unsigned int   sample_rate;
	unsigned short valid_bits_per_sample;
	unsigned short container_size;
	unsigned short number_channels;
};

/* retrieve parameters from hda format */
#define get_hdafmt_chs(fmt)	(fmt & 0xf)
#define get_hdafmt_bits(fmt)	((fmt >> 4) & 0x7)
#define get_hdafmt_rate(fmt)	((fmt >> 8) & 0x7f)
#define get_hdafmt_type(fmt)	((fmt >> 15) & 0x1)

struct ca0132_spec {
	struct snd_kcontrol_new *mixers[5];
	unsigned int num_mixers;
	const struct hda_verb *base_init_verbs;
	const struct hda_verb *base_exit_verbs;
	const struct hda_verb *init_verbs[5];
	unsigned int num_init_verbs;  /* exclude base init verbs */
	struct auto_pin_cfg autocfg;

	/* Nodes configurations */
	struct hda_multi_out multiout;
	hda_nid_t out_pins[5];
	hda_nid_t dacs[5];
	unsigned int num_outputs;
	hda_nid_t input_pins[5];
	hda_nid_t adcs[5];
	unsigned int num_inputs;
	hda_nid_t shared_mic_nid;
	hda_nid_t shared_out_nid;
	struct hda_pcm pcm_rec[5];  /* PCM information */

	/* chip access */
	struct mutex chipio_mutex; /* chip access mutex */
	u32 curr_chip_addx;

	/* DSP download related */
	enum dsp_download_state dsp_state;
	unsigned int dsp_stream_id;
	unsigned int wait_scp;
	unsigned int wait_scp_header;
	unsigned int wait_num_data;
	unsigned int scp_resp_header;
	unsigned int scp_resp_data[4];
	unsigned int scp_resp_count;

	/* mixer and effects related */
	unsigned char dmic_ctl;
	int cur_out_type;
	int cur_mic_type;
	long vnode_lvol[VNODES_COUNT];
	long vnode_rvol[VNODES_COUNT];
	long vnode_lswitch[VNODES_COUNT];
	long vnode_rswitch[VNODES_COUNT];
	long effects_switch[EFFECTS_COUNT];
	long voicefx_val;
	long cur_mic_boost;

	struct hda_codec *codec;
	struct delayed_work unsol_hp_work;

	#ifdef ENABLE_TUNING_CONTROLS
	long cur_ctl_vals[TUNING_CTLS_COUNT];
	#endif
};

/*
 * CA0132 codec access
 */
unsigned int codec_send_command(struct hda_codec *codec, hda_nid_t nid,
		unsigned int verb, unsigned int parm, unsigned int *res)
{
	unsigned int response;
	response = snd_hda_codec_read(codec, nid, 0, verb, parm);
	*res = response;

	return ((response == -1) ? -1 : 0);
}

static int codec_set_converter_format(struct hda_codec *codec, hda_nid_t nid,
		unsigned short converter_format, unsigned int *res)
{
	return codec_send_command(codec, nid, VENDOR_CHIPIO_STREAM_FORMAT,
				converter_format & 0xffff, res);
}

static int codec_set_converter_stream_channel(struct hda_codec *codec,
				hda_nid_t nid, unsigned char stream,
				unsigned char channel, unsigned int *res)
{
	unsigned char converter_stream_channel = 0;

	converter_stream_channel = (stream << 4) | (channel & 0x0f);
	return codec_send_command(codec, nid, AC_VERB_SET_CHANNEL_STREAMID,
				converter_stream_channel, res);
}

/*
 * CA0132 chip access stuffs
 */
static int chipio_send(struct hda_codec *codec,
		       unsigned int reg,
		       u32 data)
{
	unsigned int res;
	unsigned long timeout = jiffies + msecs_to_jiffies(2000);

	/* send bits of data specified by reg */
	do {
		res = snd_hda_codec_read(codec, WIDGET_CHIP_CTRL, 0,
					 reg, data);
		if (res == VENDOR_STATUS_CHIPIO_OK)
			return 0;
		msleep(1);
	} while (time_before(jiffies, timeout));

	CTASSERT(0);
	printk(KERN_ERR "CA0132 chipio timed out %x %x\n", reg, data);
	return -1;
}

static int chipio_write_addx(struct hda_codec *codec, u32 chip_addx)
{
	struct ca0132_spec *spec = codec->spec;
	int status = 0;

	if (spec->curr_chip_addx == chip_addx)
		return 0;

	/* send low 16 bits of the address */
	status = chipio_send(codec, VENDOR_CHIPIO_ADDRESS_LOW,
			     chip_addx & 0xffff);

	if (SUCCEEDED(status)) {
		/* send high 16 bits of the address */
		status = chipio_send(codec, VENDOR_CHIPIO_ADDRESS_HIGH,
				     chip_addx >> 16);
	}

	CTASSERT(SUCCEEDED(status));
	spec->curr_chip_addx = SUCCEEDED(status) ? chip_addx : ~0UL;

	return status;
}

static int chipio_write_data(struct hda_codec *codec, u32 data)
{
	struct ca0132_spec *spec = codec->spec;
	int status = 0;

	/* send low 16 bits of the data */
	status = chipio_send(codec, VENDOR_CHIPIO_DATA_LOW, data & 0xffff);

	if (SUCCEEDED(status)) {
		/* send high 16 bits of the data */
		status = chipio_send(codec, VENDOR_CHIPIO_DATA_HIGH,
				     data >> 16);
	}

	CTASSERT(SUCCEEDED(status));
	spec->curr_chip_addx = SUCCEEDED(status) ?
					(spec->curr_chip_addx + 4) : ~0UL;

	return status;
}

static int chipio_write_data_multiple(struct hda_codec *codec,
				      const u32 *data,
				      unsigned int count)
{
	int status = 0;

	if (data == NULL)
		return FAIL_MSG(-1, "chipio_write_data null ptr");

	while ((count-- != 0) && (status == 0))
		status = chipio_write_data(codec, *data++);

	return status;
}

static int chipio_read_data(struct hda_codec *codec, unsigned int *data)
{
	struct ca0132_spec *spec = codec->spec;
	int status = 0;

	/* post read */
	status = chipio_send(codec, VENDOR_CHIPIO_HIC_POST_READ, 0);

	if (SUCCEEDED(status)) {
		/* read status */
		status = chipio_send(codec, VENDOR_CHIPIO_STATUS, 0);
	}

	if (SUCCEEDED(status)) {
		/* read data */
		*data = snd_hda_codec_read(codec, WIDGET_CHIP_CTRL, 0,
					   VENDOR_CHIPIO_HIC_READ_DATA, 0);
	}

	CTASSERT(SUCCEEDED(status));
	spec->curr_chip_addx = SUCCEEDED(status) ?
					(spec->curr_chip_addx + 4) : ~0UL;

	return status;
}

static int chipio_write(struct hda_codec *codec, u32 chip_addx,
			const u32 data)
{
	struct ca0132_spec *spec = codec->spec;
	int status;

	mutex_lock(&spec->chipio_mutex);
	status = chipio_write_addx(codec, chip_addx);
	if (SUCCEEDED(status))
		status = chipio_write_data(codec, data);
	mutex_unlock(&spec->chipio_mutex);

	return status;
}

static int chipio_write_multiple(struct hda_codec *codec,
				 u32 chip_addx,
				 const u32 *data,
				 unsigned int count)
{
	struct ca0132_spec *spec = codec->spec;
	int status;

	mutex_lock(&spec->chipio_mutex);
	status = chipio_write_addx(codec, chip_addx);
	if (SUCCEEDED(status))
		status = chipio_write_data_multiple(codec, data, count);
	mutex_unlock(&spec->chipio_mutex);

	return status;
}

static int chipio_read(struct hda_codec *codec, u32 chip_addx,
		       unsigned int *data)
{
	struct ca0132_spec *spec = codec->spec;
	int status;

	mutex_lock(&spec->chipio_mutex);
	status = chipio_write_addx(codec, chip_addx);
	if (SUCCEEDED(status))
		status = chipio_read_data(codec, data);
	mutex_unlock(&spec->chipio_mutex);

	return status;
}

static void chipio_set_control_flag(struct hda_codec *codec,
				    enum control_flag_id flag_id,
				    bool flag_state)
{
	unsigned int val;
	unsigned int flag_bit;

	flag_bit = (flag_state ? 1 : 0);
	val = (flag_bit << 7) | (flag_id);
	snd_hda_codec_write(codec, WIDGET_CHIP_CTRL, 0,
			    VENDOR_CHIPIO_FLAG_SET, val);
}

static void chipio_set_control_param(struct hda_codec *codec,
		enum control_param_id param_id, int param_val)
{
	struct ca0132_spec *spec = codec->spec;
	int val;

	if ((param_id < 32) && (param_val < 8)) {
		val = (param_val << 5) | (param_id);
		snd_hda_codec_write(codec, WIDGET_CHIP_CTRL, 0,
				    VENDOR_CHIPIO_PARAM_SET, val);
	} else {
		mutex_lock(&spec->chipio_mutex);
		if (chipio_send(codec, VENDOR_CHIPIO_STATUS, 0) == 0) {
			snd_hda_codec_write(codec, WIDGET_CHIP_CTRL, 0,
					    VENDOR_CHIPIO_PARAM_EX_ID_SET,
					    param_id);
			snd_hda_codec_write(codec, WIDGET_CHIP_CTRL, 0,
					    VENDOR_CHIPIO_PARAM_EX_VALUE_SET,
					    param_val);
		} else
			CA0132_LOG("set_control_param:FAIL! id=0x%x, val=%d\n",
				   param_id, param_val);
		mutex_unlock(&spec->chipio_mutex);
	}
}

static void chipio_set_conn_rate(struct hda_codec *codec,
				int connid, enum ca0132_sample_rate rate)
{
	chipio_set_control_param(codec, CONTROL_PARAM_CONN_POINT_ID, connid);
	chipio_set_control_param(codec, CONTROL_PARAM_CONN_POINT_SAMPLE_RATE,
				 rate);
}

static void chipio_enable_clocks(struct hda_codec *codec)
{
	struct ca0132_spec *spec = codec->spec;

	mutex_lock(&spec->chipio_mutex);
	snd_hda_codec_write(codec, WIDGET_CHIP_CTRL, 0,
			    VENDOR_CHIPIO_8051_ADDRESS_LOW, 0);
	snd_hda_codec_write(codec, WIDGET_CHIP_CTRL, 0,
			    VENDOR_CHIPIO_PLL_PMU_WRITE, 0xff);
	snd_hda_codec_write(codec, WIDGET_CHIP_CTRL, 0,
			    VENDOR_CHIPIO_8051_ADDRESS_LOW, 5);
	snd_hda_codec_write(codec, WIDGET_CHIP_CTRL, 0,
			    VENDOR_CHIPIO_PLL_PMU_WRITE, 0x0b);
	snd_hda_codec_write(codec, WIDGET_CHIP_CTRL, 0,
			    VENDOR_CHIPIO_8051_ADDRESS_LOW, 6);
	snd_hda_codec_write(codec, WIDGET_CHIP_CTRL, 0,
			    VENDOR_CHIPIO_PLL_PMU_WRITE, 0xff);
	mutex_unlock(&spec->chipio_mutex);
}


/*
 * CA0132 DSP IO stuffs
 */
static int dspio_send(struct hda_codec *codec, unsigned int reg,
		      unsigned int data)
{
	unsigned int res;
	unsigned long timeout = jiffies + msecs_to_jiffies(1000);

	/* send bits of data specified by reg to dsp */
	do {
		res = snd_hda_codec_read(codec, WIDGET_DSP_CTRL, 0, reg, data);
		if ((res >= 0) && (res != VENDOR_STATUS_DSPIO_BUSY))
			return res;
		msleep(1);
	} while (time_before(jiffies, timeout));

	CTASSERT(0);
	return -1;
}

static void dspio_write_wait(struct hda_codec *codec)
{
	int status;
	unsigned long timeout = jiffies + msecs_to_jiffies(1000);

	do {
		status = snd_hda_codec_read(codec, WIDGET_DSP_CTRL, 0,
						VENDOR_DSPIO_STATUS, 0);
		if ((status == VENDOR_STATUS_DSPIO_OK) ||
		    (status == VENDOR_STATUS_DSPIO_SCP_RESPONSE_QUEUE_EMPTY))
			break;
		msleep(1);
	} while (time_before(jiffies, timeout));
}

static int dspio_write(struct hda_codec *codec, unsigned int scp_data)
{
	struct ca0132_spec *spec = codec->spec;
	int status;

	dspio_write_wait(codec);

	mutex_lock(&spec->chipio_mutex);
	status = dspio_send(codec, VENDOR_DSPIO_SCP_WRITE_DATA_LOW,
			    scp_data & 0xffff);
	if (SUCCEEDED(status))
		status = dspio_send(codec, VENDOR_DSPIO_SCP_WRITE_DATA_HIGH,
				    scp_data >> 16);
	mutex_unlock(&spec->chipio_mutex);

	/* OK, now check if the write itself has executed*/
	if (SUCCEEDED(status))
		status = snd_hda_codec_read(codec, WIDGET_DSP_CTRL, 0,
					    VENDOR_DSPIO_STATUS, 0);

	if (status == VENDOR_STATUS_DSPIO_SCP_COMMAND_QUEUE_FULL)
		CA0132_LOG("dspio_write: SCP_COMMAND_QUEUE_FULL!!!\n\n");

	return (status == VENDOR_STATUS_DSPIO_SCP_COMMAND_QUEUE_FULL) ?
			-1 : 0;
}

static int dspio_write_multiple(struct hda_codec *codec,
				unsigned int *buffer, unsigned int size)
{
	int status = 0;
	unsigned int count;

	if ((buffer == NULL))
		return -1;

	count = 0;
	while (count < size) {
		status = dspio_write(codec, *buffer++);
		if (status != 0)
			break;
		count++;
	}

	return status;
}

static int dspio_read(struct hda_codec *codec, unsigned int *data)
{
	int status;

	status = dspio_send(codec, VENDOR_DSPIO_SCP_POST_READ_DATA, 0);
	if (SUCCEEDED(status))
		status = dspio_send(codec, VENDOR_DSPIO_STATUS, 0);

	if (FAILED(status) ||
	    (status == VENDOR_STATUS_DSPIO_SCP_RESPONSE_QUEUE_EMPTY))
		return -1;

	*data = snd_hda_codec_read(codec, WIDGET_DSP_CTRL, 0,
				   VENDOR_DSPIO_SCP_READ_DATA, 0);

	return 0;
}

static int dspio_read_multiple(struct hda_codec *codec, unsigned int *buffer,
			       unsigned int *buf_size, unsigned int size_count)
{
	int status = 0;
	unsigned int size = *buf_size;
	unsigned int count;
	unsigned int skip_count;
	unsigned int dummy;

	if ((buffer == NULL))
		return -1;

	count = 0;
	while (count < size && count < size_count) {
		status = dspio_read(codec, buffer++);
		if (status != 0)
			break;
		count++;
	}

	skip_count = count;
	if (SUCCEEDED(status)) {
		while (skip_count < size) {
			status = dspio_read(codec, &dummy);
			if (status != 0)
				break;
			skip_count++;
		}
	}
	*buf_size = count;

	return status;
}

static inline unsigned int
make_scp_header(unsigned int target_id, unsigned int source_id,
		unsigned int get_flag, unsigned int req,
		unsigned int device_flag, unsigned int resp_flag,
		unsigned int error_flag, unsigned int data_size)
{
	unsigned int header = 0;

	header = (data_size & 0x1f) << 27;
	header |= (error_flag & 0x01) << 26;
	header |= (resp_flag & 0x01) << 25;
	header |= (device_flag & 0x01) << 24;
	header |= (req & 0x7f) << 17;
	header |= (get_flag & 0x01) << 16;
	header |= (source_id & 0xff) << 8;
	header |= target_id & 0xff;

	return header;
}

static inline void
extract_scp_header(unsigned int header,
		   unsigned int *target_id, unsigned int *source_id,
		   unsigned int *get_flag, unsigned int *req,
		   unsigned int *device_flag, unsigned int *resp_flag,
		   unsigned int *error_flag, unsigned int *data_size)
{
	if (data_size)
		*data_size = (header >> 27) & 0x1f;
	if (error_flag)
		*error_flag = (header >> 26) & 0x01;
	if (resp_flag)
		*resp_flag = (header >> 25) & 0x01;
	if (device_flag)
		*device_flag = (header >> 24) & 0x01;
	if (req)
		*req = (header >> 17) & 0x7f;
	if (get_flag)
		*get_flag = (header >> 16) & 0x01;
	if (source_id)
		*source_id = (header >> 8) & 0xff;
	if (target_id)
		*target_id = header & 0xff;
}

#define SCP_MAX_DATA_WORDS  (16)

/* Structure to contain any SCP message */
struct scp_msg {
	unsigned int hdr;
	unsigned int data[SCP_MAX_DATA_WORDS];
};

static void dspio_clear_response_queue(struct hda_codec *codec)
{
	unsigned int dummy = 0;
	int status = -1;

	/* clear all from the response queue */
	do {
		status = dspio_read(codec, &dummy);
	} while (status == 0);
}

static int dspio_get_response_data(struct hda_codec *codec)
{
	struct ca0132_spec *spec = codec->spec;
	unsigned int data = 0;
	unsigned int count;

	if (FAILED(dspio_read(codec, &data)))
		return -1;

	CA0132_LOG("dspio_get_response_data: 0x%08x\n", data);
	if ((data & 0x00ffffff) == spec->wait_scp_header) {
		spec->scp_resp_header = data;
		spec->scp_resp_count = data >> 27;
		count = spec->wait_num_data;
		dspio_read_multiple(codec, spec->scp_resp_data,
				    &spec->scp_resp_count, count);
		return 0;
	}

	return -1;
}

static int dspio_send_scp_message(struct hda_codec *codec,
				  unsigned char *send_buf,
				  unsigned int send_buf_size,
				  unsigned char *return_buf,
				  unsigned int return_buf_size,
				  unsigned int *bytes_returned)
{
	struct ca0132_spec *spec = codec->spec;
	int status = -1;
	unsigned int scp_send_size = 0;
	unsigned int total_size;
	bool waiting_for_resp = false;
	unsigned int header;
	struct scp_msg *ret_msg;
	unsigned int resp_src_id, resp_target_id;
	unsigned int data_size, src_id, target_id, get_flag, device_flag;

	if (bytes_returned)
		*bytes_returned = 0;

	/* get scp header from buffer */
	header = *((unsigned int *)send_buf);
	extract_scp_header(header, &target_id, &src_id, &get_flag, NULL,
			   &device_flag, NULL, NULL, &data_size);
	scp_send_size = data_size + 1;
	total_size = (scp_send_size * 4);

	CTASSERT(send_buf_size >= total_size);
	if (send_buf_size < total_size)
		return -1;

	if (get_flag || device_flag) {
		if (!return_buf || return_buf_size < 4 || !bytes_returned) {
			CTASSERT(0);
			return -1;
		}
		spec->wait_scp_header = *((unsigned int *)send_buf);

		/* swap source id with target id */
		resp_target_id = src_id;
		resp_src_id = target_id;
		spec->wait_scp_header &= 0xffff0000;
		spec->wait_scp_header |= (resp_src_id << 8) | (resp_target_id);
		spec->wait_num_data = return_buf_size/sizeof(unsigned int) - 1;
		spec->wait_scp = 1;
		waiting_for_resp = true;
	}

	status = dspio_write_multiple(codec, (unsigned int *)send_buf,
				      scp_send_size);
	if (FAILED(status)) {
		CTASSERT(0);
		spec->wait_scp = 0;
		return status;
	}

	if (waiting_for_resp) {
		unsigned long timeout = jiffies + msecs_to_jiffies(1000);
		memset(return_buf, 0, return_buf_size);
		do {
			msleep(1);
		} while (spec->wait_scp && time_before(jiffies, timeout));
		waiting_for_resp = false;
		if (!spec->wait_scp) {
			ret_msg = (struct scp_msg *)return_buf;
			memcpy(&ret_msg->hdr, &spec->scp_resp_header, 4);
			memcpy(&ret_msg->data, spec->scp_resp_data,
			       spec->wait_num_data);
			*bytes_returned = (spec->scp_resp_count + 1) * 4;
			status = 0;
		} else {
			CTASSERT(*bytes_returned == 0);
			status = -1;
		}
		spec->wait_scp = 0;
	}

	return status;
}

static int dspio_scp(struct hda_codec *codec,
		int mod_id, int req, int dir, void *data, unsigned int len,
		void *reply, unsigned int *reply_len)
{
	int status = 0;
	struct scp_msg scp_send, scp_reply;
	unsigned int ret_bytes, send_size, ret_size;
	unsigned int send_get_flag, reply_resp_flag, reply_error_flag;
	unsigned int reply_data_size;

	memset(&scp_send, 0, sizeof(scp_send));
	memset(&scp_reply, 0, sizeof(scp_reply));

	CTASSERT((len == 0 || data != NULL) && len <= SCP_MAX_DATA_WORDS);
	if ((len != 0 && data == NULL) || (len > SCP_MAX_DATA_WORDS))
		return -1;

	CTASSERT(dir != SCP_GET || reply != NULL);
	if (dir == SCP_GET && reply == NULL)
		return FAIL_MSG(-1, "dspio_scp get but has no buffer");

	CTASSERT((reply == NULL) || (reply_len != NULL && (*reply_len > 0)));
	if (reply != NULL && (reply_len == NULL || (*reply_len == 0)))
		return FAIL_MSG(-1, "dspio_scp bad resp buf len parms");

	scp_send.hdr = make_scp_header(mod_id, 0x20, (dir == SCP_GET), req,
				       0, 0, 0, len/sizeof(unsigned int));
	if (data != NULL && len > 0) {
		len = min((unsigned int)(sizeof(scp_send.data)), len);
		memcpy(scp_send.data, data, len);
	}

	ret_bytes = 0;
	send_size = sizeof(unsigned int) + len;
	status = dspio_send_scp_message(codec, (unsigned char *)&scp_send,
					send_size, (unsigned char *)&scp_reply,
					sizeof(scp_reply), &ret_bytes);

/* for debugging */
#ifdef ENABLE_CA0132_DEBUG
{
	unsigned int x, *p, i;
	unsigned int source_id;
	unsigned int target_id;
	unsigned int req;
	unsigned int get_flag;
	unsigned int resp_flag;
	unsigned int device_flag;
	unsigned int error_flag;
	unsigned int data_size;

	memcpy(&x, &scp_send.hdr, sizeof(unsigned int));
	extract_scp_header(x, &target_id, &source_id,
			   &get_flag, &req, &device_flag, &resp_flag,
			   &error_flag, &data_size);

	snd_printdd(
		"ScpDispatch ----- REQ: "
		"HDR=0x%08x "
		"SID=%04x "
		"TID=%04x "
		"Req=%04x "
		"GRDE=%d %d %d %d "
		"Siz=%d [ ",
		x,
		source_id,
		target_id,
		req,
		get_flag,
		resp_flag,
		device_flag,
		error_flag,
		data_size);

	for (x = 0; x < data_size; x++)
		snd_printdd("0x%08x ", scp_send.data[x].ui);

	snd_printdd("]\n");

	if (get_flag) {
		if (ret_bytes >= sizeof(scp_reply.hdr)) {
			memcpy(&x, &scp_reply.hdr, sizeof(unsigned int));
			extract_scp_header(x, &target_id, &source_id,
					   &get_flag, &req, &device_flag,
					   &resp_flag, &error_flag, &data_size);

			snd_printdd(
				"            ----- REP: "
				"HDR=0x%08x "
				"SID=%04x "
				"TID=%04x "
				"Req=%04x "
				"GRDE=%d %d %d %d "
				"Siz=%d [ ",
				x,
				source_id,
				target_id,
				req,
				get_flag,
				resp_flag,
				device_flag,
				error_flag,
				data_size);

			for (x = 0; x < data_size; x++)
				snd_printdd("0x%08x ", scp_reply.data[x].ui);
		} else {
			snd_printdd("REP: (too short,nbytes=%d) [", ret_bytes);

			for (p = (unsigned int *)&scp_reply, i = 0;
				i < ret_bytes; p++, i++) {
				snd_printdd("0x%04x ", *p);
			}
		}
		snd_printdd("]\n");
	}
}
#endif

	if (FAILED(status))
		return FAIL_MSG(status, "dspio_scp: send scp msg failed");

	/* extract send and reply headers members */
	extract_scp_header(scp_send.hdr, NULL, NULL, &send_get_flag,
			   NULL, NULL, NULL, NULL, NULL);
	extract_scp_header(scp_reply.hdr, NULL, NULL, NULL, NULL, NULL,
			   &reply_resp_flag, &reply_error_flag,
			   &reply_data_size);

	if (!send_get_flag)
		return 0;

	if (reply_resp_flag && !reply_error_flag) {
		ret_size = (ret_bytes - sizeof(scp_reply.hdr))
					/ sizeof(unsigned int);

		CTASSERT(dir == SCP_GET && reply != NULL && reply_len != NULL);

		if (*reply_len < ret_size*sizeof(unsigned int)) {
			status = FAIL_MSG(-1, "reply too long for buf");
		} else if (ret_size != reply_data_size) {
			status = FAIL_MSG(-1, "RetLen and HdrLen .NE.");
		} else {
			*reply_len = ret_size*sizeof(unsigned int);
			memcpy(reply, scp_reply.data, *reply_len);
		}
	} else {
		status = FAIL_MSG(-1, "reply ill-formed or errflag set");
	}

	return status;
}

static int dspio_set_param(struct hda_codec *codec, int mod_id,
			int req, void *data, unsigned int len)
{
	return dspio_scp(codec, mod_id, req, SCP_SET, data, len, NULL, NULL);
}

static int dspio_alloc_dma_chan(struct hda_codec *codec, unsigned int *dma_chan)
{
	int status = 0;
	unsigned int size = sizeof(dma_chan);

	CA0132_DSP_LOG("     dspio_alloc_dma_chan() -- begin");
	status = dspio_scp(codec, MASTERCONTROL, MASTERCONTROL_ALLOC_DMA_CHAN,
			SCP_GET, NULL, 0, dma_chan, &size);

	if (FAILED(status)) {
		CA0132_DSP_LOG("dspio_alloc_dma_chan: SCP Failed");
		return -1;
	}

	if ((*dma_chan + 1) == 0) {
		CA0132_DSP_LOG("no free dma channels to allocate");
		return -1;
	}

	CA0132_LOG("dspio_alloc_dma_chan: chan=%d\n", *dma_chan);
	CA0132_DSP_LOG("     dspio_alloc_dma_chan() -- complete");

	return status;
}

static int dspio_free_dma_chan(struct hda_codec *codec, unsigned int dma_chan)
{
	int status = 0;
	unsigned int dummy = 0;

	CA0132_DSP_LOG("     dspio_free_dma_chan() -- begin");
	CA0132_LOG("dspio_free_dma_chan: chan=%d\n", dma_chan);

	status = dspio_scp(codec, MASTERCONTROL, MASTERCONTROL_ALLOC_DMA_CHAN,
			   SCP_SET, &dma_chan, sizeof(dma_chan), NULL, &dummy);

	if (FAILED(status)) {
		CA0132_DSP_LOG("dspio_free_dma_chan: SCP Failed");
		return -1;
	}

	CA0132_DSP_LOG("     dspio_free_dma_chan() -- complete");

	return status;
}

/*
 * CA0132 DSP access stuffs
 */
static int dsp_set_run_state(struct hda_codec *codec)
{
	unsigned int dbg_ctrl_reg;
	unsigned int halt_state;
	int err;

	err = chipio_read(codec, DSP_DBGCNTL_INST_OFFSET, &dbg_ctrl_reg);
	if (err < 0)
		return err;

	halt_state = (dbg_ctrl_reg & DSP_DBGCNTL_STATE_MASK) >>
		      DSP_DBGCNTL_STATE_LOBIT;

	if (halt_state != 0) {
		dbg_ctrl_reg &= ~((halt_state << DSP_DBGCNTL_SS_LOBIT) &
				  DSP_DBGCNTL_SS_MASK);
		err = chipio_write(codec, DSP_DBGCNTL_INST_OFFSET,
				   dbg_ctrl_reg);
		if (err < 0)
			return err;

		dbg_ctrl_reg |= (halt_state << DSP_DBGCNTL_EXEC_LOBIT) &
				DSP_DBGCNTL_EXEC_MASK;
		err = chipio_write(codec, DSP_DBGCNTL_INST_OFFSET,
				   dbg_ctrl_reg);
		if (err < 0)
			return err;
	}

	return 0;
}

static int dsp_reset(struct hda_codec *codec)
{
	unsigned int res;
	int retry = 20;

	CA0132_LOG("dsp_reset\n");
	do {
		res = dspio_send(codec, VENDOR_DSPIO_DSP_INIT, 0);
		retry--;
	} while (res == -1 && retry);

	if (!retry) {
		CA0132_LOG("dsp_reset timeout\n");
		return -1;
	}

	return 0;
}

static unsigned int dsp_chip_to_dsp_addx(unsigned int chip_addx,
					bool *code, bool *yram)
{
	*code = *yram = false;

	if (UC_RANGE(chip_addx, 1)) {
		*code = true;
		return UC_OFF(chip_addx);
	} else if (X_RANGE_ALL(chip_addx, 1)) {
		return X_OFF(chip_addx);
	} else if (Y_RANGE_ALL(chip_addx, 1)) {
		*yram = true;
		return Y_OFF(chip_addx);
	}

	return (unsigned int)INVALID_CHIP_ADDRESS;
}

static bool dsp_is_dma_active(struct hda_codec *codec, unsigned int dma_chan)
{
	unsigned int dma_chnlstart_reg;

	chipio_read(codec, DSPDMAC_CHNLSTART_INST_OFFSET, &dma_chnlstart_reg);

	return ((dma_chnlstart_reg & (1 <<
			(DSPDMAC_CHNLSTART_EN_LOBIT + dma_chan))) != 0);
}

static int dsp_dma_setup_common(struct hda_codec *codec,
				unsigned int chip_addx,
				unsigned int dma_chan,
				unsigned int port_map_mask,
				bool ovly)
{
	int status = 0;
	unsigned int chnl_prop;
	unsigned int dsp_addx;
	unsigned int active;
	bool code, yram;

	CA0132_DSP_LOG("-- dsp_dma_setup_common() -- Begin ---------");

	CTASSERT(dma_chan < DSPDMAC_DMA_CFG_CHANNEL_COUNT);
	if (dma_chan >= DSPDMAC_DMA_CFG_CHANNEL_COUNT)
		return FAIL_MSG(-1, "dma chan num invalid");

	if (dsp_is_dma_active(codec, dma_chan))
		return FAIL_MSG(-1, "dma already active");

	dsp_addx = dsp_chip_to_dsp_addx(chip_addx, &code, &yram);

	if (dsp_addx == INVALID_CHIP_ADDRESS)
		return FAIL_MSG(-1, "invalid chip addr");

	chnl_prop = DSPDMAC_CHNLPROP_AC_MASK;
	active = 0;

	CA0132_DSP_LOG("   dsp_dma_setup_common()    start reg pgm");

	if (ovly) {
		status = chipio_read(codec, DSPDMAC_CHNLPROP_INST_OFFSET,
				     &chnl_prop);

		if (FAILED(status))
			return FAIL_MSG(-1, "read CHNLPROP Reg fail");

		CA0132_DSP_LOG("   dsp_dma_setup_common()    Read CHNLPROP");
	}

	if (!code)
		chnl_prop &= ~(1 << (DSPDMAC_CHNLPROP_MSPCE_LOBIT + dma_chan));
	else
		chnl_prop |=  (1 << (DSPDMAC_CHNLPROP_MSPCE_LOBIT + dma_chan));

	chnl_prop &= ~(1 << (DSPDMAC_CHNLPROP_DCON_LOBIT + dma_chan));

	status = chipio_write(codec, DSPDMAC_CHNLPROP_INST_OFFSET, chnl_prop);
	if (FAILED(status))
		return FAIL_MSG(-1, "write CHNLPROP Reg fail");

	CA0132_DSP_LOG("   dsp_dma_setup_common()    Write CHNLPROP");

	if (ovly) {
		status = chipio_read(codec, DSPDMAC_ACTIVE_INST_OFFSET,
				     &active);

		if (FAILED(status))
			return FAIL_MSG(-1, "read ACTIVE Reg fail");

		CA0132_DSP_LOG("   dsp_dma_setup_common()    Read ACTIVE");
	}

	active &= (~(1 << (DSPDMAC_ACTIVE_AAR_LOBIT + dma_chan))) &
		DSPDMAC_ACTIVE_AAR_MASK;

	status = chipio_write(codec, DSPDMAC_ACTIVE_INST_OFFSET, active);
	if (FAILED(status))
		return FAIL_MSG(-1, "write ACTIVE Reg fail");

	CA0132_DSP_LOG("   dsp_dma_setup_common()    Write ACTIVE");

	status = chipio_write(codec, DSPDMAC_AUDCHSEL_INST_OFFSET(dma_chan),
			      port_map_mask);
	if (FAILED(status))
		return FAIL_MSG(-1, "write AUDCHSEL Reg fail");

	CA0132_DSP_LOG("   dsp_dma_setup_common()    Write AUDCHSEL");

	status = chipio_write(codec, DSPDMAC_IRQCNT_INST_OFFSET(dma_chan),
			DSPDMAC_IRQCNT_BICNT_MASK | DSPDMAC_IRQCNT_CICNT_MASK);
	if (FAILED(status))
		return FAIL_MSG(-1, "write IRQCNT Reg fail");

	CA0132_DSP_LOG("   dsp_dma_setup_common()    Write IRQCNT");

	CA0132_LOG(
		   "ChipA=0x%x,DspA=0x%x,dmaCh=%u, "
		   "CHSEL=0x%x,CHPROP=0x%x,Active=0x%x\n",
		   chip_addx, dsp_addx, dma_chan,
		   port_map_mask, chnl_prop, active);

	CA0132_DSP_LOG("-- dsp_dma_setup_common() -- Complete ---------");

	return 0;
}

static int dsp_dma_setup(struct hda_codec *codec,
			unsigned int chip_addx,
			unsigned int count,
			unsigned int dma_chan)
{
	int status = 0;
	bool code, yram;
	unsigned int dsp_addx;
	unsigned int addr_field;
	unsigned int incr_field;
	unsigned int base_cnt;
	unsigned int cur_cnt;
	unsigned int dma_cfg = 0;
	unsigned int adr_ofs = 0;
	unsigned int xfr_cnt = 0;
	const unsigned int max_dma_count = 1 << (DSPDMAC_XFRCNT_BCNT_HIBIT -
						DSPDMAC_XFRCNT_BCNT_LOBIT + 1);

	CA0132_DSP_LOG("-- dsp_dma_setup() -- Begin ---------");

	CTASSERT(count <= max_dma_count);
	if (count > max_dma_count)
		return FAIL_MSG(-1, "count too big");

	dsp_addx = dsp_chip_to_dsp_addx(chip_addx, &code, &yram);
	if (dsp_addx == INVALID_CHIP_ADDRESS)
		return FAIL_MSG(-1, "invalid chip addr");

	CA0132_DSP_LOG("   dsp_dma_setup()    start reg pgm");

	addr_field = dsp_addx << DSPDMAC_DMACFG_DBADR_LOBIT;
	incr_field   = 0;

	if (!code) {
		addr_field <<= 1;
		if (yram)
			addr_field |= (1 << DSPDMAC_DMACFG_DBADR_LOBIT);

		incr_field  = (1 << DSPDMAC_DMACFG_AINCR_LOBIT);
	}

	CTASSERT((addr_field & DSPDMAC_DMACFG_DBADR_MASK) == addr_field);
	dma_cfg = addr_field + incr_field;
	status = chipio_write(codec, DSPDMAC_DMACFG_INST_OFFSET(dma_chan),
				dma_cfg);
	if (FAILED(status))
		return FAIL_MSG(-1, "write DMACFG Reg fail");

	CA0132_DSP_LOG("   dsp_dma_setup()    Write DMACFG");

	adr_ofs = (count - 1) << (DSPDMAC_DSPADROFS_BOFS_LOBIT +
							(code ? 0 : 1));
	CTASSERT((adr_ofs & DSPDMAC_DSPADROFS_BOFS_MASK) == adr_ofs);

	status = chipio_write(codec, DSPDMAC_DSPADROFS_INST_OFFSET(dma_chan),
				adr_ofs);
	if (FAILED(status))
		return FAIL_MSG(-1, "write DSPADROFS Reg fail");

	CA0132_DSP_LOG("   dsp_dma_setup()    Write DSPADROFS");

	base_cnt = (count - 1) << DSPDMAC_XFRCNT_BCNT_LOBIT;
	CTASSERT((base_cnt & DSPDMAC_XFRCNT_BCNT_MASK) == base_cnt);

	cur_cnt  = (count - 1) << DSPDMAC_XFRCNT_CCNT_LOBIT;
	CTASSERT((cur_cnt  & DSPDMAC_XFRCNT_CCNT_MASK) == cur_cnt);

	xfr_cnt = base_cnt | cur_cnt;

	status = chipio_write(codec,
				DSPDMAC_XFRCNT_INST_OFFSET(dma_chan), xfr_cnt);
	if (FAILED(status))
		return FAIL_MSG(-1, "write XFRCNT Reg fail");

	CA0132_DSP_LOG("   dsp_dma_setup()    Write XFRCNT");

	CA0132_LOG(
		   "ChipA=0x%x, cnt=0x%x, DMACFG=0x%x, "
		   "ADROFS=0x%x, XFRCNT=0x%x\n",
		   chip_addx, count, dma_cfg, adr_ofs, xfr_cnt);

	CA0132_DSP_LOG("-- dsp_dma_setup() -- Complete ---------");

	return 0;
}

static int dsp_dma_start(struct hda_codec *codec,
			 unsigned int dma_chan, bool ovly)
{
	unsigned int reg = 0;
	int status = 0;

	CA0132_DSP_LOG("-- dsp_dma_start() -- Begin ---------");

	if (ovly) {
		status = chipio_read(codec,
				     DSPDMAC_CHNLSTART_INST_OFFSET, &reg);

		if (FAILED(status))
			return FAIL_MSG(-1, "read CHNLSTART reg fail");

		CA0132_DSP_LOG("-- dsp_dma_start()    Read CHNLSTART");

		reg &= ~(DSPDMAC_CHNLSTART_EN_MASK |
				DSPDMAC_CHNLSTART_DIS_MASK);
	}

	status = chipio_write(codec, DSPDMAC_CHNLSTART_INST_OFFSET,
			reg | (1 << (dma_chan + DSPDMAC_CHNLSTART_EN_LOBIT)));
	if (FAILED(status))
		return FAIL_MSG(-1, "write CHNLSTART reg fail");

	CA0132_DSP_LOG("-- dsp_dma_start() -- Complete ---------");

	return status;
}

static int dsp_dma_stop(struct hda_codec *codec,
			unsigned int dma_chan, bool ovly)
{
	unsigned int reg = 0;
	int status = 0;

	CA0132_DSP_LOG("-- dsp_dma_stop() -- Begin ---------");

	if (ovly) {
		status = chipio_read(codec,
				     DSPDMAC_CHNLSTART_INST_OFFSET, &reg);

		if (FAILED(status))
			return FAIL_MSG(-1, "read CHNLSTART reg fail");

		CA0132_DSP_LOG("-- dsp_dma_stop()    Read CHNLSTART");
		reg &= ~(DSPDMAC_CHNLSTART_EN_MASK |
				DSPDMAC_CHNLSTART_DIS_MASK);
	}

	status = chipio_write(codec, DSPDMAC_CHNLSTART_INST_OFFSET,
			reg | (1 << (dma_chan + DSPDMAC_CHNLSTART_DIS_LOBIT)));
	if (FAILED(status))
		return FAIL_MSG(-1, "write CHNLSTART reg fail");

	CA0132_DSP_LOG("-- dsp_dma_stop() -- Complete ---------");

	return status;
}

static int dsp_allocate_router_ports(struct hda_codec *codec,
				     unsigned int num_chans,
				     unsigned int ports_per_channel,
				     unsigned int start_device,
				     unsigned int *port_map)
{
	int status = 0;
	int res;
	u8 val;

	status = chipio_send(codec, VENDOR_CHIPIO_STATUS, 0);
	if (FAILED(status))
		return status;

	val = start_device << 6;
	val |= (ports_per_channel - 1) << 4;
	val |= num_chans - 1;

	snd_hda_codec_write(codec, WIDGET_CHIP_CTRL, 0,
			    VENDOR_CHIPIO_PORT_ALLOC_CONFIG_SET,
			    val);

	snd_hda_codec_write(codec, WIDGET_CHIP_CTRL, 0,
			    VENDOR_CHIPIO_PORT_ALLOC_SET,
			    MEM_CONNID_DSP);

	status = chipio_send(codec, VENDOR_CHIPIO_STATUS, 0);
	if (FAILED(status))
		return status;

	res = snd_hda_codec_read(codec, WIDGET_CHIP_CTRL, 0,
				VENDOR_CHIPIO_PORT_ALLOC_GET, 0);

	*port_map = res;

	return (res < 0) ? -1 : 0;
}

static int dsp_free_router_ports(struct hda_codec *codec)
{
	int status = 0;

	status = chipio_send(codec, VENDOR_CHIPIO_STATUS, 0);
	if (FAILED(status))
		return status;

	snd_hda_codec_write(codec, WIDGET_CHIP_CTRL, 0,
			    VENDOR_CHIPIO_PORT_FREE_SET,
			    MEM_CONNID_DSP);

	status = chipio_send(codec, VENDOR_CHIPIO_STATUS, 0);

	return status;
}

static int dsp_allocate_ports(struct hda_codec *codec,
			unsigned int num_chans,
			unsigned int rate_multi, unsigned int *port_map)
{
	int status = -1;

	CA0132_DSP_LOG("     dsp_allocate_ports() -- begin");

	if ((rate_multi != 1) && (rate_multi != 2) && (rate_multi != 4))
		return FAIL_MSG(status, "bad rate multiple");

	status = dsp_allocate_router_ports(codec, num_chans,
					   rate_multi, 0, port_map);

	CA0132_DSP_LOG("     dsp_allocate_ports() -- complete");

	return status;
}

static int dsp_free_ports(struct hda_codec *codec)
{
	int status;

	CA0132_DSP_LOG("     dsp_free_ports() -- begin");

	status = dsp_free_router_ports(codec);
	if (FAILED(status))
		return FAIL_MSG(-1, "free router ports fail");

	CA0132_DSP_LOG("     dsp_free_ports() -- complete");

	return status;
}

static int dsp_allocate_ports_format(struct hda_codec *codec,
			const unsigned short fmt,
			unsigned int *port_map)
{
	int status = -1;
	unsigned int num_chans;

	unsigned int sample_rate_div = ((get_hdafmt_rate(fmt) >> 0) & 3) + 1;
	unsigned int sample_rate_mul = ((get_hdafmt_rate(fmt) >> 3) & 3) + 1;
	unsigned int rate_multi = sample_rate_mul / sample_rate_div;

	if ((rate_multi != 1) && (rate_multi != 2) && (rate_multi != 4))
		return FAIL_MSG(-1, "bad rate multiple");

	num_chans = get_hdafmt_chs(fmt) + 1;

	status = dsp_allocate_ports(codec, num_chans, rate_multi, port_map);

	return status;
}

/*
 *  HDA DMA engine stuffs for DSP code download
 */
struct dma_engine {
	struct hda_codec *codec;
	unsigned short m_converter_format;
	struct snd_dma_buffer *dmab;
	unsigned int buf_size;
};

enum dma_state {
	DMA_STATE_STOP  = 0,
	DMA_STATE_RUN   = 1
};

static int dma_convert_to_hda_format(
		struct hda_stream_format *stream_format,
		unsigned short *hda_format)
{
	unsigned int format_val;

	format_val = snd_hda_calc_stream_format(
				stream_format->sample_rate,
				stream_format->number_channels,
				SNDRV_PCM_FORMAT_S32_LE,
				stream_format->container_size, 0);

	if (hda_format)
		*hda_format = (unsigned short)format_val;

	return 0;
}

static int dma_reset(struct dma_engine *dma)
{
	struct hda_codec *codec = dma->codec;
	struct ca0132_spec *spec = codec->spec;
	int status;

	snd_hda_codec_load_dsp_cleanup(codec, dma->dmab);

	status = snd_hda_codec_load_dsp_prepare(codec,
			dma->m_converter_format,
			dma->buf_size,
			dma->dmab);
	if (FAILED(status))
		return status;
	spec->dsp_stream_id = status;

	return 0;
}

static int dma_set_state(struct dma_engine *dma, enum dma_state state)
{
	bool cmd;

	CA0132_LOG("dma_set_state state=%d\n", state);

	switch (state) {
	case DMA_STATE_STOP:
		cmd = false;
		break;
	case DMA_STATE_RUN:
		cmd = true;
		break;
	default:
		return 0;
	}

	snd_hda_codec_load_dsp_trigger(dma->codec, cmd);
	return 0;
}

static unsigned int dma_get_buffer_size(struct dma_engine *dma)
{
	return dma->dmab->bytes;
}

static unsigned char *dma_get_buffer_addr(struct dma_engine *dma)
{
	return dma->dmab->area;
}

static int dma_xfer(struct dma_engine *dma,
		const unsigned int *data,
		unsigned int count)
{
	memcpy(dma->dmab->area, data, count);
	return 0;
}

static void dma_get_converter_format(
		struct dma_engine *dma,
		unsigned short *format)
{
	if (format)
		*format = dma->m_converter_format;
}

static unsigned int dma_get_stream_id(struct dma_engine *dma)
{
	struct ca0132_spec *spec = dma->codec->spec;

	return spec->dsp_stream_id;
}


/*
 * CA0132 chip DSP image segment stuffs
 */
struct dsp_image_seg {
	u32 magic;
	u32 chip_addr;
	u32 count;
	u32 data[0];
};

static const u32 g_magic_value = 0x4c46584d;
static const u32 g_chip_addr_magic_value = 0xFFFFFF01;

static bool is_valid(const struct dsp_image_seg *p)
{
	return p->magic == g_magic_value;
}

static bool is_hci_prog_list_seg(const struct dsp_image_seg *p)
{
	return g_chip_addr_magic_value == p->chip_addr;
}

static bool is_last(const struct dsp_image_seg *p)
{
	return p->count == 0;
}

static size_t dsp_sizeof(const struct dsp_image_seg *p)
{
	return sizeof(*p) + p->count*sizeof(u32);
}

static const struct dsp_image_seg *get_next_seg_ptr(
				const struct dsp_image_seg *p)
{
	return (struct dsp_image_seg *)((unsigned char *)(p) + dsp_sizeof(p));
}

/*
 * CA0132 chip DSP trannsfer stuffs.  For DSP download.
 */
#define INVALID_DMA_CHANNEL (~0UL)

static int dspxfr_hci_write(struct hda_codec *codec,
			const struct dsp_image_seg *fls)
{
	int status = 0;
	const u32 *data;
	unsigned int count;

	CTASSERT(fls != NULL && fls->chip_addr == g_chip_addr_magic_value);
	if (fls == NULL || fls->chip_addr != g_chip_addr_magic_value)
		return FAIL_MSG(-1, "hci_write invalid params");

	count = fls->count;
	data = (u32 *)(fls->data);
	while (SUCCEEDED(status) && count >= 2) {
		status = chipio_write(codec, data[0], data[1]);
		if (FAILED(status))
			status = FAIL_MSG(status, "hci_write chipio failed");
		count -= 2;
		data  += 2;
	}
	return status;
}

static int dspxfr_one_seg(struct hda_codec *codec,
			const struct dsp_image_seg *fls,
			unsigned int reloc,
			struct dma_engine *dma_engine,
			unsigned int dma_chan,
			unsigned int port_map_mask,
			bool ovly)
{
	int status = 0;
	bool comm_dma_setup_done = false;
	const unsigned int *data;
	unsigned int chip_addx;
	unsigned int words_to_write;
	unsigned int buffer_size_words;
	unsigned char *buffer_addx;
	unsigned short hda_format;
	unsigned int sample_rate_div;
	unsigned int sample_rate_mul;
	unsigned int num_chans;
	unsigned int hda_frame_size_words;
	unsigned int remainder_words;
	const u32 *data_remainder;
	u32 chip_addx_remainder;
	unsigned int run_size_words;
	const struct dsp_image_seg *hci_write = NULL;
	unsigned long timeout;
	bool dma_active;

	CTASSERT(fls != NULL);
	if (fls == NULL)
		return -1;
	if (is_hci_prog_list_seg(fls)) {
		hci_write = fls;
		fls = get_next_seg_ptr(fls);
	}

	if (hci_write && (!fls || is_last(fls))) {
		CA0132_LOG("hci_write\n");
		return dspxfr_hci_write(codec, hci_write);
	}

	if (fls == NULL || dma_engine == NULL || port_map_mask == 0) {
		CA0132_LOG("Invalid Params\n");
		return -1;
	}

	data = fls->data;
	chip_addx = fls->chip_addr,
	words_to_write = fls->count;

	if (!words_to_write)
		return hci_write ? dspxfr_hci_write(codec, hci_write) : 0;
	if (reloc)
		chip_addx = (chip_addx & (0xFFFF0000 << 2)) + (reloc << 2);

	if (!UC_RANGE(chip_addx, words_to_write) &&
	    !X_RANGE_ALL(chip_addx, words_to_write) &&
	    !Y_RANGE_ALL(chip_addx, words_to_write)) {
		CA0132_LOG("Invalid chip_addx Params\n");
		return -1;
	}

	buffer_size_words = (unsigned int)dma_get_buffer_size(dma_engine) /
					sizeof(u32);

	buffer_addx = dma_get_buffer_addr(dma_engine);

	if (buffer_addx == NULL)
		status = FAIL_MSG(-1, "dma_engine buffer NULL");

	dma_get_converter_format(dma_engine, &hda_format);
	sample_rate_div = ((get_hdafmt_rate(hda_format) >> 0) & 3) + 1;
	sample_rate_mul = ((get_hdafmt_rate(hda_format) >> 3) & 3) + 1;
	num_chans = get_hdafmt_chs(hda_format) + 1;

	hda_frame_size_words = ((sample_rate_div == 0) ? 0 :
			(num_chans * sample_rate_mul / sample_rate_div));

	buffer_size_words = min(buffer_size_words,
				(unsigned int)(UC_RANGE(chip_addx, 1) ?
				65536 : 32768));
	buffer_size_words -= buffer_size_words % hda_frame_size_words;
	CA0132_LOG(
		   "chpadr=0x%08x frmsz=%u nchan=%u "
		   "rate_mul=%u div=%u bufsz=%u\n",
		   chip_addx, hda_frame_size_words, num_chans,
		   sample_rate_mul, sample_rate_div, buffer_size_words);

	CTASSERT(SUCCEEDED(status));
	CTASSERT(buffer_addx != NULL);
	CTASSERT(buffer_size_words >= hda_frame_size_words);
	CTASSERT(hda_frame_size_words > 0);

	if ((buffer_addx == NULL) || (hda_frame_size_words == 0) ||
	    (buffer_size_words < hda_frame_size_words)) {
		status = FAIL_MSG(-1, "dspxfr_one_seg:failed");
	}

	if (FAILED(status))
		return status;

	remainder_words = words_to_write % hda_frame_size_words;
	data_remainder = data;
	chip_addx_remainder = chip_addx;

	data += remainder_words;
	chip_addx += remainder_words*sizeof(u32);
	words_to_write -= remainder_words;

	while ((words_to_write != 0) && SUCCEEDED(status)) {
		run_size_words = min(buffer_size_words, words_to_write);
		CA0132_LOG("dspxfr (seg loop)cnt=%u rs=%u remainder=%u\n",
			   words_to_write, run_size_words, remainder_words);
		dma_xfer(dma_engine, data, run_size_words*sizeof(u32));
		if (!comm_dma_setup_done && SUCCEEDED(status)) {
			status = dsp_dma_stop(codec, dma_chan, ovly);
			if (SUCCEEDED(status))
				status = dsp_dma_setup_common(codec, chip_addx,
						dma_chan, port_map_mask, ovly);
			comm_dma_setup_done = true;
		}

		if (SUCCEEDED(status))
			status = dsp_dma_setup(codec, chip_addx,
						run_size_words, dma_chan);
		if (SUCCEEDED(status))
			status = dsp_dma_start(codec, dma_chan, ovly);
		if (SUCCEEDED(status) && !dsp_is_dma_active(codec, dma_chan))
			status = FAIL_MSG(-1, "dspxfr:DMA did not start");
		if (SUCCEEDED(status))
			status = dma_set_state(dma_engine, DMA_STATE_RUN);
		if (SUCCEEDED(status)) {
			if (remainder_words != 0) {
				status = chipio_write_multiple(codec,
							chip_addx_remainder,
							data_remainder,
							remainder_words);
				remainder_words = 0;
			}
			if (hci_write) {
				status = dspxfr_hci_write(codec, hci_write);
				hci_write = NULL;
			}

			timeout = jiffies + msecs_to_jiffies(2000);
			do {
				dma_active = dsp_is_dma_active(codec, dma_chan);
				if (!dma_active)
					break;
				msleep(1);
			} while (time_before(jiffies, timeout));
			if (dma_active)
				break;
			CA0132_DSP_LOG("+++++ DMA complete");
			dma_set_state(dma_engine, DMA_STATE_STOP);
			status = dma_reset(dma_engine);
			if (FAILED(status))
				break;
		}

		CTASSERT(run_size_words <= words_to_write);
		data += run_size_words;
		chip_addx += run_size_words*sizeof(u32);
		words_to_write -= run_size_words;
	}

	if (SUCCEEDED(status) && (remainder_words != 0)) {
		status = chipio_write_multiple(codec, chip_addx_remainder,
					data_remainder, remainder_words);
	}

	return status;
}

static int dspxfr_image(struct hda_codec *codec,
			const struct dsp_image_seg *fls_data,
			unsigned int reloc, struct hda_stream_format *format,
			bool ovly)
{
	struct ca0132_spec *spec = codec->spec;
	int status = 0;
	unsigned short hda_format = 0;
	unsigned int response;
	unsigned char stream_id = 0;
	struct dma_engine *dma_engine;
	unsigned int dma_chan;
	unsigned int port_map_mask;

	CTASSERT(fls_data != NULL);
	if (fls_data == NULL)
		return -1;

	dma_engine = kzalloc(sizeof(*dma_engine), GFP_KERNEL);
	if (!dma_engine) {
		status = -ENOMEM;
		goto exit;
	}

	dma_engine->dmab = kzalloc(sizeof(*dma_engine->dmab), GFP_KERNEL);
	if (!dma_engine->dmab) {
		status = -ENOMEM;
		goto free_dma_engine;
	}

	dma_engine->codec = codec;
	dma_convert_to_hda_format(format, &hda_format);
	dma_engine->m_converter_format = hda_format;
	dma_engine->buf_size = (ovly ? DSP_DMA_WRITE_BUFLEN_OVLY :
			DSP_DMA_WRITE_BUFLEN_INIT) * 2;

	dma_chan = 0;
	do {
		status = codec_set_converter_format(codec, WIDGET_CHIP_CTRL,
						hda_format, &response);

		if (FAILED(status)) {
			status = FAIL_MSG(status, "set converter format fail");
			goto free_dma_engine_and_dmab;
		}

		status = snd_hda_codec_load_dsp_prepare(codec,
					dma_engine->m_converter_format,
					dma_engine->buf_size,
					dma_engine->dmab);
		if (FAILED(status))
			break;
		spec->dsp_stream_id = status;

		if (ovly) {
			status = dspio_alloc_dma_chan(codec, &dma_chan);
			if (FAILED(status)) {
				status = FAIL_MSG(status, "alloc dmachan fail");
				dma_chan = (unsigned int)INVALID_DMA_CHANNEL;
				break;
			}
		}

		port_map_mask = 0;
		status = dsp_allocate_ports_format(codec, hda_format,
						&port_map_mask);
		if (FAILED(status)) {
			status = FAIL_MSG(status, "alloc parts fail");
			break;
		}

		stream_id = dma_get_stream_id(dma_engine);
		status = codec_set_converter_stream_channel(codec,
				WIDGET_CHIP_CTRL, stream_id, 0, &response);
		if (FAILED(status)) {
			status = FAIL_MSG(status, "set stream chan fail");
			break;
		}

		while (SUCCEEDED(status) && (fls_data != NULL) &&
					!is_last(fls_data)) {
			if (!is_valid(fls_data)) {
				status = FAIL_MSG(-1, "FLS check fail");
				break;
			}
			status = dspxfr_one_seg(codec, fls_data, reloc,
						dma_engine, dma_chan,
						port_map_mask, ovly);
			if (is_hci_prog_list_seg(fls_data))
				fls_data = get_next_seg_ptr(fls_data);

			CTASSERT(fls_data != NULL);
			if ((fls_data != NULL) && !is_last(fls_data))
				fls_data = get_next_seg_ptr(fls_data);

			CTASSERT(fls_data != NULL);
		}

		if (port_map_mask != 0)
			status = dsp_free_ports(codec);

		if (FAILED(status))
			break;

		status = codec_set_converter_stream_channel(codec,
					WIDGET_CHIP_CTRL, 0, 0, &response);
	} while (0);

	if (ovly && (dma_chan != INVALID_DMA_CHANNEL))
		status = dspio_free_dma_chan(codec, dma_chan);

	snd_hda_codec_load_dsp_cleanup(codec, dma_engine->dmab);
free_dma_engine_and_dmab:
	kfree(dma_engine->dmab);
free_dma_engine:
	kfree(dma_engine);
exit:
	return status;
}

/*
 * CA0132 DSP download stuffs.
 */
static void dspload_post_setup(struct hda_codec *codec)
{
	CA0132_DSP_LOG("---- dspload_post_setup ------");

	/*set DSP speaker to 2.0 configuration*/
	chipio_write(codec, XRAM_XRAM_INST_OFFSET(0x18), 0x08080080);
	chipio_write(codec, XRAM_XRAM_INST_OFFSET(0x19), 0x3f800000);

	/*update write pointer*/
	chipio_write(codec, XRAM_XRAM_INST_OFFSET(0x29), 0x00000002);
}

static int dspload_image(struct hda_codec *codec,
			const struct dsp_image_seg *fls,
			bool ovly,
			unsigned int reloc,
			bool autostart,
			int router_chans)
{
	int status = 0;
	struct hda_stream_format stream_format;

	CA0132_DSP_LOG("---- dspload_image begin ------");
	if (router_chans == 0) {
		if (!ovly)
			router_chans = DMA_TRANSFER_FRAME_SIZE_NWORDS;
		else
			router_chans = DMA_OVERLAY_FRAME_SIZE_NWORDS;
	}

	stream_format.sample_rate = 48000;
	stream_format.number_channels = (unsigned short)router_chans;

	while (stream_format.number_channels > 16) {
		stream_format.sample_rate *= 2;
		stream_format.number_channels /= 2;
	}

	stream_format.container_size = 32;
	stream_format.valid_bits_per_sample = 32;

	do {
		CA0132_DSP_LOG("Ready to program DMA");
		if (!ovly)
			status = dsp_reset(codec);

		if (FAILED(status))
			break;

		CA0132_DSP_LOG("dsp_reset() complete");
		status = dspxfr_image(codec, fls, reloc, &stream_format, ovly);

		if (FAILED(status))
			break;

		CA0132_DSP_LOG("dspxfr_image() complete");
		if (autostart && !ovly) {
			dspload_post_setup(codec);
			status = dsp_set_run_state(codec);
		}

		CA0132_DSP_LOG("LOAD FINISHED");
	} while (0);

	return status;
}

static int dspload_get_speakereq_addx(struct hda_codec *codec,
				unsigned int *x,
				unsigned int *y)
{
	int status = 0;
	struct { unsigned short y, x; } speakereq_info;
	unsigned int size = sizeof(speakereq_info);

	CA0132_DSP_LOG("dspload_get_speakereq_addx() -- begin");
	status = dspio_scp(codec, MASTERCONTROL,
			MASTERCONTROL_QUERY_SPEAKER_EQ_ADDRESS,
			SCP_GET, NULL, 0, &speakereq_info, &size);

	if (FAILED(status)) {
		CA0132_DSP_LOG("dspload_get_speakereq_addx: SCP Failed");
		return -1;
	}

	*x = speakereq_info.x;
	*y = speakereq_info.y;
	CA0132_LOG("dspload_get_speakereq_addx: X=0x%x Y=0x%x\n", *x, *y);

	CA0132_DSP_LOG("dspload_get_speakereq_addx() -- complete");

	return status;
}

static const struct firmware *fw_speq;
static const struct firmware *fw_efx;

static int request_firmware_cached(const struct firmware **firmware_p,
	const char *name, struct device *device)
{
	if (*firmware_p)
		return 0;  /* already loaded */
	return request_firmware(firmware_p, name, device);
}

static void release_cached_firmware(void)
{
	if (fw_speq) {
		release_firmware(fw_speq);
		fw_speq = NULL;
	}
	if (fw_efx) {
		release_firmware(fw_efx);
		fw_efx = NULL;
	}
}

static int dspload_speakereq(struct hda_codec *codec)
{
	int status = 0;
	const struct dsp_image_seg *image;
	unsigned int x, y;

	CA0132_DSP_LOG("dspload_speakereq() -- begin");

	if (request_firmware_cached(&fw_speq, SPEQ_FILE,
				    codec->bus->card->dev) != 0)
		return -1;

	image = (struct dsp_image_seg *)(fw_speq->data);

	status = dspload_get_speakereq_addx(codec, &x, &y);
	if (FAILED(status))
		goto done;

	status = dspload_image(codec, image, 1, y, 0, 8);

done:
	CA0132_DSP_LOG("dspload_speakereq() -- complete");

	return status;
}

static bool dspload_is_loaded(struct hda_codec *codec)
{
	unsigned int data = 0;
	int status = 0;

	status = chipio_read(codec, 0x40004, &data);
	if (FAILED(status) || (data != 1))
		return false;

	return true;
}

static bool dspload_wait_loaded(struct hda_codec *codec)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(2000);

	do {
		if (dspload_is_loaded(codec)) {
			pr_info("DOWNLOAD OK :-) DSP IS RUNNING.\n");
			return true;
		}
		msleep(1);
	} while (time_before(jiffies, timeout));

	pr_err("DOWNLOAD FAILED!!! DSP IS NOT RUNNING.\n");
	return false;
}

/*
 * PCM stuffs.
 */
static void ca0132_setup_stream(struct hda_codec *codec, hda_nid_t nid,
				u32 stream_tag,
				int channel_id, int format)
{
	unsigned int oldval, newval;

	if (!nid)
		return;

	CA0132_LOG(
		   "ca0132_setup_stream: NID=0x%x, stream=0x%x, "
		   "channel=%d, format=0x%x\n",
		   nid, stream_tag, channel_id, format);

	/* update the format-id if changed */
	oldval = snd_hda_codec_read(codec, nid, 0,
				    AC_VERB_GET_STREAM_FORMAT, 0);
	if (oldval != format) {
		msleep(20);
		snd_hda_codec_write(codec, nid, 0,
				    AC_VERB_SET_STREAM_FORMAT,
				    format);
	}

	oldval = snd_hda_codec_read(codec, nid, 0, AC_VERB_GET_CONV, 0);
	newval = (stream_tag << 4) | channel_id;
	if (oldval != newval) {
		snd_hda_codec_write(codec, nid, 0,
				    AC_VERB_SET_CHANNEL_STREAMID,
				    newval);
	}
}

static void ca0132_cleanup_stream(struct hda_codec *codec, hda_nid_t nid)
{
	unsigned int val;

	if (!nid)
		return;

	CA0132_LOG("ca0132_cleanup_stream: NID=0x%x\n",	nid);

	val = snd_hda_codec_read(codec, nid, 0, AC_VERB_GET_CONV, 0);
	if (!val)
		return;

	snd_hda_codec_write(codec, nid, 0, AC_VERB_SET_STREAM_FORMAT, 0);
	snd_hda_codec_write(codec, nid, 0, AC_VERB_SET_CHANNEL_STREAMID, 0);
}

/*
 * PCM playbacks
 */
static unsigned int ca0132_get_playback_latency(struct hda_codec *codec)
{
	struct ca0132_spec *spec = codec->spec;
	unsigned int latency = DSP_PLAYBACK_INIT_LATENCY;

	if (!dspload_is_loaded(codec))
		return 0;

	if (spec->effects_switch[PLAY_ENHANCEMENT - EFFECT_START_NID]) {
		if ((spec->effects_switch[SURROUND - EFFECT_START_NID]) ||
		    (spec->effects_switch[DIALOG_PLUS - EFFECT_START_NID]))
			latency += DSP_PLAY_ENHANCEMENT_LATENCY;
	}

	if (spec->cur_out_type == SPEAKER_OUT)
		latency += DSP_SPEAKER_OUT_LATENCY;
	return latency;
}

static int ca0132_playback_pcm_prepare(struct hda_pcm_stream *hinfo,
				       struct hda_codec *codec,
				       unsigned int stream_tag,
				       unsigned int format,
				       struct snd_pcm_substream *substream)
{
	struct ca0132_spec *spec = codec->spec;
	struct snd_pcm_runtime *runtime = substream->runtime;
	unsigned int latency = ca0132_get_playback_latency(codec);

	if (spec->dsp_state == DSP_DOWNLOADING) {
		spec->dsp_stream_id = stream_tag;
		return 0;
	}

	runtime->delay = bytes_to_frames(runtime, (latency * runtime->rate *
					runtime->byte_align) / 1000);

	ca0132_setup_stream(codec, spec->dacs[0], stream_tag, 0, format);
	return 0;
}

static int ca0132_playback_pcm_cleanup(struct hda_pcm_stream *hinfo,
				       struct hda_codec *codec,
				       struct snd_pcm_substream *substream)
{
	struct ca0132_spec *spec = codec->spec;

	if (spec->dsp_state == DSP_DOWNLOADING)
		return 0;

	/*Allow stream some time to flush effects tail*/
	msleep(50);

	ca0132_cleanup_stream(codec, spec->dacs[0]);
	return 0;
}

/*
 * PCM capture
 */

static unsigned int ca0132_get_capture_latency(struct hda_codec *codec)
{
	struct ca0132_spec *spec = codec->spec;
	unsigned int latency = DSP_CAPTURE_INIT_LATENCY;

	if (!dspload_is_loaded(codec))
		return 0;

	if (spec->effects_switch[CRYSTAL_VOICE - EFFECT_START_NID])
		latency += DSP_CRYSTAL_VOICE_LATENCY;
	return latency;
}

static int ca0132_capture_pcm_prepare(struct hda_pcm_stream *hinfo,
				      struct hda_codec *codec,
				      unsigned int stream_tag,
				      unsigned int format,
				      struct snd_pcm_substream *substream)
{
	struct ca0132_spec *spec = codec->spec;
	struct snd_pcm_runtime *runtime = substream->runtime;
	unsigned int latency = ca0132_get_capture_latency(codec);

	if (spec->dsp_state == DSP_DOWNLOADING)
		return 0;

	runtime->delay = bytes_to_frames(runtime, (latency * runtime->rate *
					runtime->byte_align) / 1000);

	ca0132_setup_stream(codec, hinfo->nid, stream_tag, 0, format);
	return 0;
}

static int ca0132_capture_pcm_cleanup(struct hda_pcm_stream *hinfo,
				      struct hda_codec *codec,
				      struct snd_pcm_substream *substream)
{
	struct ca0132_spec *spec = codec->spec;

	if (spec->dsp_state == DSP_DOWNLOADING)
		return 0;

	ca0132_cleanup_stream(codec, hinfo->nid);
	return 0;
}


/*
 * Controls stuffs.
 */

/*
 * Mixer controls helpers.
 */
#define CA0132_CODEC_VOL_MONO(xname, nid, channel, dir) \
	{ .iface = SNDRV_CTL_ELEM_IFACE_MIXER, \
	  .name = xname, \
	  .subdevice = HDA_SUBDEV_AMP_FLAG, \
	  .access = SNDRV_CTL_ELEM_ACCESS_READWRITE | \
			SNDRV_CTL_ELEM_ACCESS_TLV_READ | \
			SNDRV_CTL_ELEM_ACCESS_TLV_CALLBACK, \
	  .info = ca0132_volume_info, \
	  .get = ca0132_volume_get, \
	  .put = ca0132_volume_put, \
	  .tlv = { .c = ca0132_volume_tlv }, \
	  .private_value = HDA_COMPOSE_AMP_VAL(nid, channel, 0, dir) }

#define CA0132_CODEC_MUTE_MONO(xname, nid, channel, dir) \
	{ .iface = SNDRV_CTL_ELEM_IFACE_MIXER, \
	  .name = xname, \
	  .subdevice = HDA_SUBDEV_AMP_FLAG, \
	  .info = snd_hda_mixer_amp_switch_info, \
	  .get = ca0132_switch_get, \
	  .put = ca0132_switch_put, \
	  .private_value = HDA_COMPOSE_AMP_VAL(nid, channel, 0, dir) }

/* stereo */
#define CA0132_CODEC_VOL(xname, nid, dir) \
	CA0132_CODEC_VOL_MONO(xname, nid, 3, dir)
#define CA0132_CODEC_MUTE(xname, nid, dir) \
	CA0132_CODEC_MUTE_MONO(xname, nid, 3, dir)


/* The followings are for tuning of products */
#ifdef ENABLE_TUNING_CONTROLS

static unsigned int voice_focus_vals_lookup[] = {
0x41A00000, 0x41A80000, 0x41B00000, 0x41B80000, 0x41C00000, 0x41C80000,
0x41D00000, 0x41D80000, 0x41E00000, 0x41E80000, 0x41F00000, 0x41F80000,
0x42000000, 0x42040000, 0x42080000, 0x420C0000, 0x42100000, 0x42140000,
0x42180000, 0x421C0000, 0x42200000, 0x42240000, 0x42280000, 0x422C0000,
0x42300000, 0x42340000, 0x42380000, 0x423C0000, 0x42400000, 0x42440000,
0x42480000, 0x424C0000, 0x42500000, 0x42540000, 0x42580000, 0x425C0000,
0x42600000, 0x42640000, 0x42680000, 0x426C0000, 0x42700000, 0x42740000,
0x42780000, 0x427C0000, 0x42800000, 0x42820000, 0x42840000, 0x42860000,
0x42880000, 0x428A0000, 0x428C0000, 0x428E0000, 0x42900000, 0x42920000,
0x42940000, 0x42960000, 0x42980000, 0x429A0000, 0x429C0000, 0x429E0000,
0x42A00000, 0x42A20000, 0x42A40000, 0x42A60000, 0x42A80000, 0x42AA0000,
0x42AC0000, 0x42AE0000, 0x42B00000, 0x42B20000, 0x42B40000, 0x42B60000,
0x42B80000, 0x42BA0000, 0x42BC0000, 0x42BE0000, 0x42C00000, 0x42C20000,
0x42C40000, 0x42C60000, 0x42C80000, 0x42CA0000, 0x42CC0000, 0x42CE0000,
0x42D00000, 0x42D20000, 0x42D40000, 0x42D60000, 0x42D80000, 0x42DA0000,
0x42DC0000, 0x42DE0000, 0x42E00000, 0x42E20000, 0x42E40000, 0x42E60000,
0x42E80000, 0x42EA0000, 0x42EC0000, 0x42EE0000, 0x42F00000, 0x42F20000,
0x42F40000, 0x42F60000, 0x42F80000, 0x42FA0000, 0x42FC0000, 0x42FE0000,
0x43000000, 0x43010000, 0x43020000, 0x43030000, 0x43040000, 0x43050000,
0x43060000, 0x43070000, 0x43080000, 0x43090000, 0x430A0000, 0x430B0000,
0x430C0000, 0x430D0000, 0x430E0000, 0x430F0000, 0x43100000, 0x43110000,
0x43120000, 0x43130000, 0x43140000, 0x43150000, 0x43160000, 0x43170000,
0x43180000, 0x43190000, 0x431A0000, 0x431B0000, 0x431C0000, 0x431D0000,
0x431E0000, 0x431F0000, 0x43200000, 0x43210000, 0x43220000, 0x43230000,
0x43240000, 0x43250000, 0x43260000, 0x43270000, 0x43280000, 0x43290000,
0x432A0000, 0x432B0000, 0x432C0000, 0x432D0000, 0x432E0000, 0x432F0000,
0x43300000, 0x43310000, 0x43320000, 0x43330000, 0x43340000
};

static unsigned int mic_svm_vals_lookup[] = {
0x00000000, 0x3C23D70A, 0x3CA3D70A, 0x3CF5C28F, 0x3D23D70A, 0x3D4CCCCD,
0x3D75C28F, 0x3D8F5C29, 0x3DA3D70A, 0x3DB851EC, 0x3DCCCCCD, 0x3DE147AE,
0x3DF5C28F, 0x3E051EB8, 0x3E0F5C29, 0x3E19999A, 0x3E23D70A, 0x3E2E147B,
0x3E3851EC, 0x3E428F5C, 0x3E4CCCCD, 0x3E570A3D, 0x3E6147AE, 0x3E6B851F,
0x3E75C28F, 0x3E800000, 0x3E851EB8, 0x3E8A3D71, 0x3E8F5C29, 0x3E947AE1,
0x3E99999A, 0x3E9EB852, 0x3EA3D70A, 0x3EA8F5C3, 0x3EAE147B, 0x3EB33333,
0x3EB851EC, 0x3EBD70A4, 0x3EC28F5C, 0x3EC7AE14, 0x3ECCCCCD, 0x3ED1EB85,
0x3ED70A3D, 0x3EDC28F6, 0x3EE147AE, 0x3EE66666, 0x3EEB851F, 0x3EF0A3D7,
0x3EF5C28F, 0x3EFAE148, 0x3F000000, 0x3F028F5C, 0x3F051EB8, 0x3F07AE14,
0x3F0A3D71, 0x3F0CCCCD, 0x3F0F5C29, 0x3F11EB85, 0x3F147AE1, 0x3F170A3D,
0x3F19999A, 0x3F1C28F6, 0x3F1EB852, 0x3F2147AE, 0x3F23D70A, 0x3F266666,
0x3F28F5C3, 0x3F2B851F, 0x3F2E147B, 0x3F30A3D7, 0x3F333333, 0x3F35C28F,
0x3F3851EC, 0x3F3AE148, 0x3F3D70A4, 0x3F400000, 0x3F428F5C, 0x3F451EB8,
0x3F47AE14, 0x3F4A3D71, 0x3F4CCCCD, 0x3F4F5C29, 0x3F51EB85, 0x3F547AE1,
0x3F570A3D, 0x3F59999A, 0x3F5C28F6, 0x3F5EB852, 0x3F6147AE, 0x3F63D70A,
0x3F666666, 0x3F68F5C3, 0x3F6B851F, 0x3F6E147B, 0x3F70A3D7, 0x3F733333,
0x3F75C28F, 0x3F7851EC, 0x3F7AE148, 0x3F7D70A4, 0x3F800000
};

static unsigned int equalizer_vals_lookup[] = {
0xC1C00000, 0xC1B80000, 0xC1B00000, 0xC1A80000, 0xC1A00000, 0xC1980000,
0xC1900000, 0xC1880000, 0xC1800000, 0xC1700000, 0xC1600000, 0xC1500000,
0xC1400000, 0xC1300000, 0xC1200000, 0xC1100000, 0xC1000000, 0xC0E00000,
0xC0C00000, 0xC0A00000, 0xC0800000, 0xC0400000, 0xC0000000, 0xBF800000,
0x00000000, 0x3F800000, 0x40000000, 0x40400000, 0x40800000, 0x40A00000,
0x40C00000, 0x40E00000, 0x41000000, 0x41100000, 0x41200000, 0x41300000,
0x41400000, 0x41500000, 0x41600000, 0x41700000, 0x41800000, 0x41880000,
0x41900000, 0x41980000, 0x41A00000, 0x41A80000, 0x41B00000, 0x41B80000,
0x41C00000
};

static int tuning_ctl_set(struct hda_codec *codec, hda_nid_t nid,
			  unsigned int *lookup, int idx)
{
	int i = 0;

	for (i = 0; i < TUNING_CTLS_COUNT; i++)
		if (nid == ca0132_tuning_ctls[i].nid)
			break;

	snd_hda_power_up(codec);
	dspio_set_param(codec, ca0132_tuning_ctls[i].mid,
			ca0132_tuning_ctls[i].req,
			&(lookup[idx]), sizeof(unsigned int));
	snd_hda_power_down(codec);

	return 1;
}

static int tuning_ctl_get(struct snd_kcontrol *kcontrol,
			  struct snd_ctl_elem_value *ucontrol)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);
	struct ca0132_spec *spec = codec->spec;
	hda_nid_t nid = get_amp_nid(kcontrol);
	long *valp = ucontrol->value.integer.value;
	int idx = nid - TUNING_CTL_START_NID;

	*valp = spec->cur_ctl_vals[idx];
	return 0;
}

static int voice_focus_ctl_info(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_info *uinfo)
{
	int chs = get_amp_channels(kcontrol);
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = chs == 3 ? 2 : 1;
	uinfo->value.integer.min = 20;
	uinfo->value.integer.max = 180;
	uinfo->value.integer.step = 1;

	return 0;
}

static int voice_focus_ctl_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);
	struct ca0132_spec *spec = codec->spec;
	hda_nid_t nid = get_amp_nid(kcontrol);
	long *valp = ucontrol->value.integer.value;
	int idx;

	idx = nid - TUNING_CTL_START_NID;
	/* any change? */
	if (spec->cur_ctl_vals[idx] == *valp)
		return 0;

	spec->cur_ctl_vals[idx] = *valp;

	idx = *valp - 20;
	tuning_ctl_set(codec, nid, voice_focus_vals_lookup, idx);

	return 1;
}

static int mic_svm_ctl_info(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_info *uinfo)
{
	int chs = get_amp_channels(kcontrol);
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = chs == 3 ? 2 : 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 100;
	uinfo->value.integer.step = 1;

	return 0;
}

static int mic_svm_ctl_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);
	struct ca0132_spec *spec = codec->spec;
	hda_nid_t nid = get_amp_nid(kcontrol);
	long *valp = ucontrol->value.integer.value;
	int idx;

	idx = nid - TUNING_CTL_START_NID;
	/* any change? */
	if (spec->cur_ctl_vals[idx] == *valp)
		return 0;

	spec->cur_ctl_vals[idx] = *valp;

	idx = *valp;
	tuning_ctl_set(codec, nid, mic_svm_vals_lookup, idx);

	return 0;
}

static int equalizer_ctl_info(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_info *uinfo)
{
	int chs = get_amp_channels(kcontrol);
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = chs == 3 ? 2 : 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 48;
	uinfo->value.integer.step = 1;

	return 0;
}

static int equalizer_ctl_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);
	struct ca0132_spec *spec = codec->spec;
	hda_nid_t nid = get_amp_nid(kcontrol);
	long *valp = ucontrol->value.integer.value;
	int idx;

	idx = nid - TUNING_CTL_START_NID;
	/* any change? */
	if (spec->cur_ctl_vals[idx] == *valp)
		return 0;

	spec->cur_ctl_vals[idx] = *valp;

	idx = *valp;
	tuning_ctl_set(codec, nid, equalizer_vals_lookup, idx);

	return 1;
}

DECLARE_TLV_DB_SCALE(voice_focus_db_scale, 2000, 100, 0);
DECLARE_TLV_DB_SCALE(eq_db_scale, -2400, 100, 0);

static int add_tuning_control(struct hda_codec *codec,
				hda_nid_t pnid, hda_nid_t nid,
				const char *name, int dir)
{
	char namestr[44];
	int type = dir ? HDA_INPUT : HDA_OUTPUT;
	struct snd_kcontrol_new knew =
		HDA_CODEC_VOLUME_MONO(namestr, nid, 1, 0, type);

	knew.access = SNDRV_CTL_ELEM_ACCESS_READWRITE |
			SNDRV_CTL_ELEM_ACCESS_TLV_READ;
	knew.tlv.c = 0;
	knew.tlv.p = 0;
	switch (pnid) {
	case VOICE_FOCUS:
		knew.info = voice_focus_ctl_info;
		knew.get = tuning_ctl_get;
		knew.put = voice_focus_ctl_put;
		knew.tlv.p = voice_focus_db_scale;
		break;
	case MIC_SVM:
		knew.info = mic_svm_ctl_info;
		knew.get = tuning_ctl_get;
		knew.put = mic_svm_ctl_put;
		break;
	case EQUALIZER:
		knew.info = equalizer_ctl_info;
		knew.get = tuning_ctl_get;
		knew.put = equalizer_ctl_put;
		knew.tlv.p = eq_db_scale;
		break;
	default:
		return 0;
	}
	knew.private_value =
		HDA_COMPOSE_AMP_VAL(nid, 1, 0, type);
	sprintf(namestr, "%s %s Volume", name, dirstr[dir]);
	return snd_hda_ctl_add(codec, nid, snd_ctl_new1(&knew, codec));
}

static int add_tuning_ctls(struct hda_codec *codec)
{
	int i;
	int err;

	for (i = 0; i < TUNING_CTLS_COUNT; i++) {
		err = add_tuning_control(codec,
					ca0132_tuning_ctls[i].parent_nid,
					ca0132_tuning_ctls[i].nid,
					ca0132_tuning_ctls[i].name,
					ca0132_tuning_ctls[i].direct);
		if (err < 0)
			return err;
	}

	return 0;
}

static void ca0132_init_tuning_defaults(struct hda_codec *codec)
{
	struct ca0132_spec *spec = codec->spec;
	int i;

	/* Wedge Angle defaults to 30.  10 below is 30 - 20.  20 is min. */
	spec->cur_ctl_vals[WEDGE_ANGLE - TUNING_CTL_START_NID] = 10;
	/* SVM level defaults to 0.74. */
	spec->cur_ctl_vals[SVM_LEVEL - TUNING_CTL_START_NID] = 74;

	/* EQ defaults to 0dB. */
	for (i = 2; i < TUNING_CTLS_COUNT; i++)
		spec->cur_ctl_vals[i] = 24;
}
#endif

static int ca0132_select_out(struct hda_codec *codec)
{
	struct ca0132_spec *spec = codec->spec;
	unsigned int pin_ctl;
	int jack_present;
	int auto_jack;
	unsigned int tmp;
	int err;

	CA0132_LOG("ca0132_select_out\n");

	snd_hda_power_up(codec);

	auto_jack = spec->vnode_lswitch[VNID_HP_ASEL - VNODE_START_NID];

	if (auto_jack)
		jack_present = snd_hda_jack_detect(codec, spec->out_pins[1]);
	else
		jack_present =
			spec->vnode_lswitch[VNID_HP_SEL - VNODE_START_NID];

	if (jack_present)
		spec->cur_out_type = HEADPHONE_OUT;
	else
		spec->cur_out_type = SPEAKER_OUT;

	if (spec->cur_out_type == SPEAKER_OUT) {
		CA0132_LOG("ca0132_select_out speaker\n");
		/*speaker out config*/
		tmp = FLOAT_ONE;
		err = dspio_set_param(codec, 0x80, 0x04,
					&tmp, sizeof(unsigned int));
		if (err < 0)
			goto exit;
		/*enable speaker EQ*/
		tmp = FLOAT_ONE;
		err = dspio_set_param(codec, 0x8f, 0x00,
					&tmp, sizeof(unsigned int));
		if (err < 0)
			goto exit;

		/* Setup EAPD */
		snd_hda_codec_write(codec, spec->out_pins[1], 0,
				    VENDOR_CHIPIO_EAPD_SEL_SET, 0x02);
		snd_hda_codec_write(codec, spec->out_pins[0], 0,
				    AC_VERB_SET_EAPD_BTLENABLE, 0x00);
		snd_hda_codec_write(codec, spec->out_pins[0], 0,
				    VENDOR_CHIPIO_EAPD_SEL_SET, 0x00);
		snd_hda_codec_write(codec, spec->out_pins[0], 0,
				    AC_VERB_SET_EAPD_BTLENABLE, 0x02);

		/* disable headphone node */
		pin_ctl = snd_hda_codec_read(codec, spec->out_pins[1], 0,
					AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
		snd_hda_codec_write(codec, spec->out_pins[1], 0,
				    AC_VERB_SET_PIN_WIDGET_CONTROL,
				    pin_ctl & 0xBF);
		/* enable speaker node */
		pin_ctl = snd_hda_codec_read(codec, spec->out_pins[0], 0,
					     AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
		snd_hda_codec_write(codec, spec->out_pins[0], 0,
				    AC_VERB_SET_PIN_WIDGET_CONTROL,
				    pin_ctl | 0x40);
	} else {
		CA0132_LOG("ca0132_select_out hp\n");
		/*headphone out config*/
		tmp = FLOAT_ZERO;
		err = dspio_set_param(codec, 0x80, 0x04,
					&tmp, sizeof(unsigned int));
		if (err < 0)
			goto exit;
		/*disable speaker EQ*/
		tmp = FLOAT_ZERO;
		err = dspio_set_param(codec, 0x8f, 0x00,
					&tmp, sizeof(unsigned int));
		if (err < 0)
			goto exit;

		/* Setup EAPD */
		snd_hda_codec_write(codec, spec->out_pins[0], 0,
				    VENDOR_CHIPIO_EAPD_SEL_SET, 0x00);
		snd_hda_codec_write(codec, spec->out_pins[0], 0,
				    AC_VERB_SET_EAPD_BTLENABLE, 0x00);
		snd_hda_codec_write(codec, spec->out_pins[1], 0,
				    VENDOR_CHIPIO_EAPD_SEL_SET, 0x02);
		snd_hda_codec_write(codec, spec->out_pins[0], 0,
				    AC_VERB_SET_EAPD_BTLENABLE, 0x02);

		/* disable speaker*/
		pin_ctl = snd_hda_codec_read(codec, spec->out_pins[0], 0,
					AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
		snd_hda_codec_write(codec, spec->out_pins[0], 0,
				    AC_VERB_SET_PIN_WIDGET_CONTROL,
				    pin_ctl & 0xBF);
		/* enable headphone*/
		pin_ctl = snd_hda_codec_read(codec, spec->out_pins[1], 0,
					AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
		snd_hda_codec_write(codec, spec->out_pins[1], 0,
				    AC_VERB_SET_PIN_WIDGET_CONTROL,
				    pin_ctl | 0x40);
	}

exit:
	snd_hda_power_down(codec);

	return err < 0 ? err : 0;
}

static void ca0132_unsol_hp_delayed(struct work_struct *work)
{
	struct ca0132_spec *spec = container_of(
		to_delayed_work(work), struct ca0132_spec, unsol_hp_work);
	ca0132_select_out(spec->codec);
	snd_hda_jack_report_sync(spec->codec);
}

static void ca0132_set_dmic(struct hda_codec *codec, int enable);
static int ca0132_mic_boost_set(struct hda_codec *codec, long val);
static int ca0132_effects_set(struct hda_codec *codec, hda_nid_t nid, long val);

static int ca0132_set_vipsource(struct hda_codec *codec, int val)
{
	struct ca0132_spec *spec = codec->spec;
	unsigned int tmp;

	if (!dspload_is_loaded(codec))
		return 0;

	/* if CrystalVoice if off, vipsource should be 0 */
	if (!spec->effects_switch[CRYSTAL_VOICE - EFFECT_START_NID]
			|| (val == 0)) {
		chipio_set_control_param(codec, CONTROL_PARAM_VIP_SOURCE, 0);
		chipio_set_conn_rate(codec, MEM_CONNID_MICIN1, SR_96_000);
		chipio_set_conn_rate(codec, MEM_CONNID_MICOUT1, SR_96_000);
		if (spec->cur_mic_type == DIGITAL_MIC)
			tmp = FLOAT_TWO;
		else
			tmp = FLOAT_ONE;
		dspio_set_param(codec, 0x80, 0x00, &tmp, sizeof(unsigned int));
		tmp = FLOAT_ZERO;
		dspio_set_param(codec, 0x80, 0x05, &tmp, sizeof(unsigned int));
	} else {
		chipio_set_conn_rate(codec, MEM_CONNID_MICIN1, SR_16_000);
		chipio_set_conn_rate(codec, MEM_CONNID_MICOUT1, SR_16_000);
		if (spec->cur_mic_type == DIGITAL_MIC)
			tmp = FLOAT_TWO;
		else
			tmp = FLOAT_ONE;
		dspio_set_param(codec, 0x80, 0x00, &tmp, sizeof(unsigned int));
		tmp = FLOAT_ONE;
		dspio_set_param(codec, 0x80, 0x05, &tmp, sizeof(unsigned int));
		msleep(20);
		chipio_set_control_param(codec, CONTROL_PARAM_VIP_SOURCE, val);
	}

	return 1;
}

static int ca0132_select_mic(struct hda_codec *codec)
{
	struct ca0132_spec *spec = codec->spec;
	int jack_present;
	int auto_jack;

	CA0132_LOG("ca0132_select_mic\n");

	snd_hda_power_up(codec);

	auto_jack = spec->vnode_lswitch[VNID_AMIC1_ASEL - VNODE_START_NID];

	if (auto_jack)
		jack_present = snd_hda_jack_detect(codec, spec->input_pins[0]);
	else
		jack_present =
			spec->vnode_lswitch[VNID_AMIC1_SEL - VNODE_START_NID];

	if (jack_present)
		spec->cur_mic_type = LINE_MIC_IN;
	else
		spec->cur_mic_type = DIGITAL_MIC;

	if (spec->cur_mic_type == DIGITAL_MIC) {
		/* enable digital Mic */
		chipio_set_conn_rate(codec, MEM_CONNID_DMIC, SR_32_000);
		ca0132_set_dmic(codec, 1);
		ca0132_mic_boost_set(codec, 0);
		/* set voice focus */
		ca0132_effects_set(codec, VOICE_FOCUS,
				   spec->effects_switch
				   [VOICE_FOCUS - EFFECT_START_NID]);
	} else {
		/* disable digital Mic */
		chipio_set_conn_rate(codec, MEM_CONNID_DMIC, SR_96_000);
		ca0132_set_dmic(codec, 0);
		ca0132_mic_boost_set(codec, spec->cur_mic_boost);
		/* disable voice focus */
		ca0132_effects_set(codec, VOICE_FOCUS, 0);
	}

	snd_hda_power_down(codec);

	return 0;
}

/*
 * Check if VNODE settings take effect immediately.
 */
static bool ca0132_is_vnode_effective(struct hda_codec *codec,
				     hda_nid_t vnid,
				     hda_nid_t *shared_nid)
{
	struct ca0132_spec *spec = codec->spec;
	hda_nid_t nid;
	bool effective = false;

	switch (vnid) {
	case VNID_SPK:
		nid = spec->shared_out_nid;
		effective = true;
		break;
	case VNID_MIC:
		nid = spec->shared_mic_nid;
		effective = true;
		break;
	default:
		break;
	}

	if (effective && shared_nid)
		*shared_nid = nid;

	return effective;
}

/*
 * The following functions are control change helpers.
 * They return 0 if no changed.  Return 1 if changed.
 */
static int ca0132_voicefx_set(struct hda_codec *codec, int enable)
{
	struct ca0132_spec *spec = codec->spec;
	unsigned int tmp;

	/* based on CrystalVoice state to enable VoiceFX. */
	if (enable) {
		tmp = spec->effects_switch[CRYSTAL_VOICE - EFFECT_START_NID] ?
			FLOAT_ONE : FLOAT_ZERO;
	} else {
		tmp = FLOAT_ZERO;
	}

	dspio_set_param(codec, ca0132_voicefx.mid,
			ca0132_voicefx.reqs[0], &tmp, sizeof(unsigned int));

	return 1;
}

static void ca0132_update_latency(struct hda_codec *codec,
				  unsigned int direction)
{
	struct ca0132_spec *spec = codec->spec;
	unsigned int latency;
	struct snd_pcm_substream *substr;
	struct snd_pcm_runtime *runtime;

	if (spec->dsp_state == DSP_DOWNLOADING)
		return;

	latency = (direction == SNDRV_PCM_STREAM_PLAYBACK)
			? ca0132_get_playback_latency(codec)
			: ca0132_get_capture_latency(codec);

	substr = codec->pcm_info->pcm->streams[direction].substream;
	while (substr) {
		runtime = substr->runtime;
		/* update latency only when runtime is setup */
		if (runtime && runtime->status->state != SNDRV_PCM_STATE_OPEN)
			runtime->delay = bytes_to_frames(runtime,
					(latency * runtime->rate *
					runtime->byte_align) / 1000);
		substr = substr->next;
	}
}

static int ca0132_effects_set(struct hda_codec *codec, hda_nid_t nid, long val)
{
	struct ca0132_spec *spec = codec->spec;
	unsigned int on;
	int num_fx = OUT_EFFECTS_COUNT + IN_EFFECTS_COUNT;
	int err = 0;
	int idx = nid - EFFECT_START_NID;

	if ((idx < 0) || (idx >= num_fx))
		return 0; /* no changed */

	/* for out effect, qualify with PE */
	if ((nid >= OUT_EFFECT_START_NID) && (nid < OUT_EFFECT_END_NID)) {
		/* if PE if off, turn off out effects. */
		if (!spec->effects_switch[PLAY_ENHANCEMENT - EFFECT_START_NID])
			val = 0;
	}

	/* for in effect, qualify with CrystalVoice */
	if ((nid >= IN_EFFECT_START_NID) && (nid < IN_EFFECT_END_NID)) {
		/* if CrystalVoice if off, turn off in effects. */
		if (!spec->effects_switch[CRYSTAL_VOICE - EFFECT_START_NID])
			val = 0;

		/* Voice Focus applies to 2-ch Mic, Digital Mic */
		if ((nid == VOICE_FOCUS) && (spec->cur_mic_type != DIGITAL_MIC))
			val = 0;
	}

	CA0132_LOG("ca0132_effect_set: nid=0x%x, val=%ld\n", nid, val);

	on = (val == 0) ? FLOAT_ZERO : FLOAT_ONE;
	err = dspio_set_param(codec, ca0132_effects[idx].mid,
				ca0132_effects[idx].reqs[0],
				&on, sizeof(unsigned int));

	ca0132_update_latency(codec, SNDRV_PCM_STREAM_PLAYBACK);
	ca0132_update_latency(codec, SNDRV_PCM_STREAM_CAPTURE);

	if (err < 0)
		return 0; /* no changed */

	return 1;
}

static int ca0132_pe_switch_set(struct hda_codec *codec)
{
	struct ca0132_spec *spec = codec->spec;
	hda_nid_t nid;
	int i, ret = 0;

	CA0132_LOG("ca0132_pe_switch_set: val=%ld\n",
		   spec->effects_switch[PLAY_ENHANCEMENT - EFFECT_START_NID]);

	i = OUT_EFFECT_START_NID - EFFECT_START_NID;
	nid = OUT_EFFECT_START_NID;
	/* PE affects all out effects */
	for (; nid < OUT_EFFECT_END_NID; nid++, i++)
		ret |= ca0132_effects_set(codec, nid, spec->effects_switch[i]);

	return ret;
}

/* Check if Mic1 is streaming, if so, stop streaming */
static int stop_mic1(struct hda_codec *codec)
{
	struct ca0132_spec *spec = codec->spec;
	unsigned int oldval = snd_hda_codec_read(codec, spec->adcs[0], 0,
						 AC_VERB_GET_CONV, 0);
	if (oldval != 0)
		snd_hda_codec_write(codec, spec->adcs[0], 0,
				    AC_VERB_SET_CHANNEL_STREAMID,
				    0);
	return oldval;
}

/* Resume Mic1 streaming if it was stopped. */
static void resume_mic1(struct hda_codec *codec, unsigned int oldval)
{
	struct ca0132_spec *spec = codec->spec;
	/* Restore the previous stream and channel */
	if (oldval != 0)
		snd_hda_codec_write(codec, spec->adcs[0], 0,
				    AC_VERB_SET_CHANNEL_STREAMID,
				    oldval);
}

static int ca0132_cvoice_switch_set(struct hda_codec *codec)
{
	struct ca0132_spec *spec = codec->spec;
	hda_nid_t nid;
	int i, ret = 0;
	unsigned int oldval;

	CA0132_LOG("ca0132_cvoice_switch_set: val=%ld\n",
		   spec->effects_switch[CRYSTAL_VOICE - EFFECT_START_NID]);

	i = IN_EFFECT_START_NID - EFFECT_START_NID;
	nid = IN_EFFECT_START_NID;
	/* CrystalVoice affects all in effects */
	for (; nid < IN_EFFECT_END_NID; nid++, i++)
		ret |= ca0132_effects_set(codec, nid, spec->effects_switch[i]);

	/* including VoiceFX */
	ret |= ca0132_voicefx_set(codec, (spec->voicefx_val ? 1 : 0));

	/* set correct vipsource */
	oldval = stop_mic1(codec);
	ret |= ca0132_set_vipsource(codec, 1);
	resume_mic1(codec, oldval);
	return ret;
}

static int ca0132_mic_boost_set(struct hda_codec *codec, long val)
{
	struct ca0132_spec *spec = codec->spec;
	int ret = 0;

	if (val) /* on */
		ret = snd_hda_codec_amp_update(codec, spec->input_pins[0], 0,
					HDA_INPUT, 0, HDA_AMP_VOLMASK, 3);
	else /* off */
		ret = snd_hda_codec_amp_update(codec, spec->input_pins[0], 0,
					HDA_INPUT, 0, HDA_AMP_VOLMASK, 0);

	return ret;
}

static int ca0132_vnode_switch_set(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);
	hda_nid_t nid = get_amp_nid(kcontrol);
	hda_nid_t shared_nid = 0;
	bool effective;
	int ret = 0;
	struct ca0132_spec *spec = codec->spec;
	int auto_jack;

	if (nid == VNID_HP_SEL) {
		auto_jack =
			spec->vnode_lswitch[VNID_HP_ASEL - VNODE_START_NID];
		if (!auto_jack)
			ca0132_select_out(codec);
		return 1;
	}

	if (nid == VNID_AMIC1_SEL) {
		auto_jack =
			spec->vnode_lswitch[VNID_AMIC1_ASEL - VNODE_START_NID];
		if (!auto_jack)
			ca0132_select_mic(codec);
		return 1;
	}

	if (nid == VNID_HP_ASEL) {
		ca0132_select_out(codec);
		return 1;
	}

	if (nid == VNID_AMIC1_ASEL) {
		ca0132_select_mic(codec);
		return 1;
	}

	/* if effective conditions, then update hw immediately. */
	effective = ca0132_is_vnode_effective(codec, nid, &shared_nid);
	if (effective) {
		int dir = get_amp_direction(kcontrol);
		int ch = get_amp_channels(kcontrol);
		unsigned long pval;

		mutex_lock(&codec->control_mutex);
		pval = kcontrol->private_value;
		kcontrol->private_value = HDA_COMPOSE_AMP_VAL(shared_nid, ch,
								0, dir);
		ret = snd_hda_mixer_amp_switch_put(kcontrol, ucontrol);
		kcontrol->private_value = pval;
		mutex_unlock(&codec->control_mutex);
	}

	return ret;
}
/* End of control change helpers. */

static int ca0132_voicefx_info(struct snd_kcontrol *kcontrol,
				 struct snd_ctl_elem_info *uinfo)
{
	unsigned int items = sizeof(ca0132_voicefx_presets)
				/ sizeof(struct ct_voicefx_preset);

	uinfo->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	uinfo->count = 1;
	uinfo->value.enumerated.items = items;
	if (uinfo->value.enumerated.item >= items)
		uinfo->value.enumerated.item = items - 1;
	strcpy(uinfo->value.enumerated.name,
	       ca0132_voicefx_presets[uinfo->value.enumerated.item].name);
	return 0;
}

static int ca0132_voicefx_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);
	struct ca0132_spec *spec = codec->spec;

	ucontrol->value.enumerated.item[0] = spec->voicefx_val;
	return 0;
}

static int ca0132_voicefx_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);
	struct ca0132_spec *spec = codec->spec;
	int i, err = 0;
	int sel = ucontrol->value.enumerated.item[0];
	unsigned int items = sizeof(ca0132_voicefx_presets)
				/ sizeof(struct ct_voicefx_preset);

	if (sel >= items)
		return 0;

	CA0132_LOG("ca0132_voicefx_put: sel=%d, preset=%s\n",
		   sel, ca0132_voicefx_presets[sel].name);

	/*
	 * Idx 0 is default.
	 * Default needs to qualify with CrystalVoice state.
	 */
	for (i = 0; i < VOICEFX_MAX_PARAM_COUNT; i++) {
		err = dspio_set_param(codec, ca0132_voicefx.mid,
				ca0132_voicefx.reqs[i],
				&(ca0132_voicefx_presets[sel].vals[i]),
				sizeof(unsigned int));
		if (err < 0)
			break;
	}

	if (err >= 0) {
		spec->voicefx_val = sel;
		/* enable voice fx */
		ca0132_voicefx_set(codec, (sel ? 1 : 0));
	}

	return 1;
}

static int ca0132_switch_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);
	struct ca0132_spec *spec = codec->spec;
	hda_nid_t nid = get_amp_nid(kcontrol);
	int ch = get_amp_channels(kcontrol);
	long *valp = ucontrol->value.integer.value;

	/* vnode */
	if ((nid >= VNODE_START_NID) && (nid < VNODE_END_NID)) {
		if (ch & 1) {
			*valp = spec->vnode_lswitch[nid - VNODE_START_NID];
			valp++;
		}
		if (ch & 2) {
			*valp = spec->vnode_rswitch[nid - VNODE_START_NID];
			valp++;
		}
		return 0;
	}

	/* effects, include PE and CrystalVoice */
	if ((nid >= EFFECT_START_NID) && (nid < EFFECT_END_NID)) {
		*valp = spec->effects_switch[nid - EFFECT_START_NID];
		return 0;
	}

	/* mic boost */
	if (nid == spec->input_pins[0]) {
		*valp = spec->cur_mic_boost;
		return 0;
	}

	return 0;
}

static int ca0132_switch_put(struct snd_kcontrol *kcontrol,
			     struct snd_ctl_elem_value *ucontrol)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);
	struct ca0132_spec *spec = codec->spec;
	hda_nid_t nid = get_amp_nid(kcontrol);
	int ch = get_amp_channels(kcontrol);
	long *valp = ucontrol->value.integer.value;
	int changed = 1;

	CA0132_LOG("ca0132_switch_put: nid=0x%x, val=%ld\n", nid, *valp);

	snd_hda_power_up(codec);
	/* vnode */
	if ((nid >= VNODE_START_NID) && (nid < VNODE_END_NID)) {
		if (ch & 1) {
			spec->vnode_lswitch[nid - VNODE_START_NID] = *valp;
			valp++;
		}
		if (ch & 2) {
			spec->vnode_rswitch[nid - VNODE_START_NID] = *valp;
			valp++;
		}
		changed = ca0132_vnode_switch_set(kcontrol, ucontrol);
		goto exit;
	}

	/* PE */
	if (nid == PLAY_ENHANCEMENT) {
		spec->effects_switch[nid - EFFECT_START_NID] = *valp;
		changed = ca0132_pe_switch_set(codec);
		goto exit;
	}

	/* CrystalVoice */
	if (nid == CRYSTAL_VOICE) {
		spec->effects_switch[nid - EFFECT_START_NID] = *valp;
		changed = ca0132_cvoice_switch_set(codec);
		goto exit;
	}

	/* out and in effects */
	if (((nid >= OUT_EFFECT_START_NID) && (nid < OUT_EFFECT_END_NID)) ||
	    ((nid >= IN_EFFECT_START_NID) && (nid < IN_EFFECT_END_NID))) {
		spec->effects_switch[nid - EFFECT_START_NID] = *valp;
		changed = ca0132_effects_set(codec, nid, *valp);
		goto exit;
	}

	/* mic boost */
	if (nid == spec->input_pins[0]) {
		spec->cur_mic_boost = *valp;

		/* Mic boost does not apply to Digital Mic */
		if (spec->cur_mic_type != DIGITAL_MIC)
			changed = ca0132_mic_boost_set(codec, *valp);
		goto exit;
	}

exit:
	snd_hda_power_down(codec);
	return changed;
}

/*
 * Volume related
 */
static int ca0132_volume_info(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_info *uinfo)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);
	struct ca0132_spec *spec = codec->spec;
	hda_nid_t nid = get_amp_nid(kcontrol);
	int ch = get_amp_channels(kcontrol);
	int dir = get_amp_direction(kcontrol);
	unsigned long pval;
	int err;

	switch (nid) {
	case VNID_SPK:
		/* follow shared_out info */
		nid = spec->shared_out_nid;
		mutex_lock(&codec->control_mutex);
		pval = kcontrol->private_value;
		kcontrol->private_value = HDA_COMPOSE_AMP_VAL(nid, ch, 0, dir);
		err = snd_hda_mixer_amp_volume_info(kcontrol, uinfo);
		kcontrol->private_value = pval;
		mutex_unlock(&codec->control_mutex);
		break;
	case VNID_MIC:
		/* follow shared_mic info */
		nid = spec->shared_mic_nid;
		mutex_lock(&codec->control_mutex);
		pval = kcontrol->private_value;
		kcontrol->private_value = HDA_COMPOSE_AMP_VAL(nid, ch, 0, dir);
		err = snd_hda_mixer_amp_volume_info(kcontrol, uinfo);
		kcontrol->private_value = pval;
		mutex_unlock(&codec->control_mutex);
		break;
	default:
		err = snd_hda_mixer_amp_volume_info(kcontrol, uinfo);
	}
	return err;
}

static int ca0132_volume_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);
	struct ca0132_spec *spec = codec->spec;
	hda_nid_t nid = get_amp_nid(kcontrol);
	int ch = get_amp_channels(kcontrol);
	long *valp = ucontrol->value.integer.value;

	/* store the left and right volume */
	if (ch & 1) {
		*valp = spec->vnode_lvol[nid - VNODE_START_NID];
		valp++;
	}
	if (ch & 2) {
		*valp = spec->vnode_rvol[nid - VNODE_START_NID];
		valp++;
	}
	return 0;
}

static int ca0132_volume_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);
	struct ca0132_spec *spec = codec->spec;
	hda_nid_t nid = get_amp_nid(kcontrol);
	int ch = get_amp_channels(kcontrol);
	long *valp = ucontrol->value.integer.value;
	hda_nid_t shared_nid = 0;
	bool effective;
	int changed = 1;

	/* store the left and right volume */
	if (ch & 1) {
		spec->vnode_lvol[nid - VNODE_START_NID] = *valp;
		valp++;
	}
	if (ch & 2) {
		spec->vnode_rvol[nid - VNODE_START_NID] = *valp;
		valp++;
	}

	/* if effective conditions, then update hw immediately. */
	effective = ca0132_is_vnode_effective(codec, nid, &shared_nid);
	if (effective) {
		int dir = get_amp_direction(kcontrol);
		unsigned long pval;

		snd_hda_power_up(codec);
		mutex_lock(&codec->control_mutex);
		pval = kcontrol->private_value;
		kcontrol->private_value = HDA_COMPOSE_AMP_VAL(shared_nid, ch,
								0, dir);
		changed = snd_hda_mixer_amp_volume_put(kcontrol, ucontrol);
		kcontrol->private_value = pval;
		mutex_unlock(&codec->control_mutex);
		snd_hda_power_down(codec);
	}

	return changed;
}

static int ca0132_volume_tlv(struct snd_kcontrol *kcontrol, int op_flag,
			     unsigned int size, unsigned int __user *tlv)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);
	struct ca0132_spec *spec = codec->spec;
	hda_nid_t nid = get_amp_nid(kcontrol);
	int ch = get_amp_channels(kcontrol);
	int dir = get_amp_direction(kcontrol);
	unsigned long pval;
	int err;

	switch (nid) {
	case VNID_SPK:
		/* follow shared_out tlv */
		nid = spec->shared_out_nid;
		mutex_lock(&codec->control_mutex);
		pval = kcontrol->private_value;
		kcontrol->private_value = HDA_COMPOSE_AMP_VAL(nid, ch, 0, dir);
		err = snd_hda_mixer_amp_tlv(kcontrol, op_flag, size, tlv);
		kcontrol->private_value = pval;
		mutex_unlock(&codec->control_mutex);
		break;
	case VNID_MIC:
		/* follow shared_mic tlv */
		nid = spec->shared_mic_nid;
		mutex_lock(&codec->control_mutex);
		pval = kcontrol->private_value;
		kcontrol->private_value = HDA_COMPOSE_AMP_VAL(nid, ch, 0, dir);
		err = snd_hda_mixer_amp_tlv(kcontrol, op_flag, size, tlv);
		kcontrol->private_value = pval;
		mutex_unlock(&codec->control_mutex);
		break;
	default:
		err = snd_hda_mixer_amp_tlv(kcontrol, op_flag, size, tlv);
	}
	return err;
}

static int add_fx_switch(struct hda_codec *codec, hda_nid_t nid,
			 const char *pfx, int dir)
{
	char namestr[44];
	int type = dir ? HDA_INPUT : HDA_OUTPUT;
	struct snd_kcontrol_new knew =
		CA0132_CODEC_MUTE_MONO(namestr, nid, 1, type);
	sprintf(namestr, "%s %s Switch", pfx, dirstr[dir]);
	return snd_hda_ctl_add(codec, nid, snd_ctl_new1(&knew, codec));
}

static int add_voicefx(struct hda_codec *codec)
{
	struct snd_kcontrol_new knew =
		HDA_CODEC_MUTE_MONO(ca0132_voicefx.name,
				    VOICEFX, 1, 0, HDA_INPUT);
	knew.info = ca0132_voicefx_info;
	knew.get = ca0132_voicefx_get;
	knew.put = ca0132_voicefx_put;
	return snd_hda_ctl_add(codec, VOICEFX, snd_ctl_new1(&knew, codec));
}

/*
 * When changing Node IDs for Mixer Controls below, make sure to update
 * Node IDs in ca0132_config() as well.
 */
static struct snd_kcontrol_new ca0132_mixer[] = {
	CA0132_CODEC_VOL("Master Playback Volume", VNID_SPK, HDA_OUTPUT),
	CA0132_CODEC_MUTE("Master Playback Switch", VNID_SPK, HDA_OUTPUT),
	CA0132_CODEC_VOL("Capture Volume", VNID_MIC, HDA_INPUT),
	CA0132_CODEC_MUTE("Capture Switch", VNID_MIC, HDA_INPUT),
	HDA_CODEC_VOLUME("Analog-Mic2 Capture Volume", 0x08, 0, HDA_INPUT),
	HDA_CODEC_MUTE("Analog-Mic2 Capture Switch", 0x08, 0, HDA_INPUT),
	HDA_CODEC_VOLUME("What U Hear Capture Volume", 0x0a, 0, HDA_INPUT),
	HDA_CODEC_MUTE("What U Hear Capture Switch", 0x0a, 0, HDA_INPUT),
	CA0132_CODEC_MUTE_MONO("Mic1-Boost (30dB) Capture Switch",
			       0x12, 1, HDA_INPUT),
	CA0132_CODEC_MUTE_MONO("HP/Speaker Playback Switch",
			       VNID_HP_SEL, 1, HDA_OUTPUT),
	CA0132_CODEC_MUTE_MONO("AMic1/DMic Capture Switch",
			       VNID_AMIC1_SEL, 1, HDA_INPUT),
	CA0132_CODEC_MUTE_MONO("HP/Speaker Auto Detect Playback Switch",
			       VNID_HP_ASEL, 1, HDA_OUTPUT),
	CA0132_CODEC_MUTE_MONO("AMic1/DMic Auto Detect Capture Switch",
			       VNID_AMIC1_ASEL, 1, HDA_INPUT),
	{ } /* end */
};

static int ca0132_build_controls(struct hda_codec *codec)
{
	struct ca0132_spec *spec = codec->spec;
	int i, num_fx;
	int err = 0;

	/* Add Mixer controls */
	for (i = 0; i < spec->num_mixers; i++) {
		err = snd_hda_add_new_ctls(codec, spec->mixers[i]);
		if (err < 0)
			return err;
	}

	/* Add in and out effects controls.
	 * VoiceFX, PE and CrystalVoice are added separately.
	 */
	num_fx = OUT_EFFECTS_COUNT + IN_EFFECTS_COUNT;
	for (i = 0; i < num_fx; i++) {
		err = add_fx_switch(codec, ca0132_effects[i].nid,
				    ca0132_effects[i].name,
				    ca0132_effects[i].direct);
		if (err < 0)
			return err;
	}

	err = add_fx_switch(codec, PLAY_ENHANCEMENT, "PlayEnhancement", 0);
	if (err < 0)
		return err;

	err = add_fx_switch(codec, CRYSTAL_VOICE, "CrystalVoice", 1);
	if (err < 0)
		return err;

	add_voicefx(codec);

	#ifdef ENABLE_TUNING_CONTROLS
	add_tuning_ctls(codec);
	#endif

	err = snd_hda_jack_add_kctls(codec, &spec->autocfg);
	if (err < 0)
		return err;

	return 0;
}

/*
 * PCM
 */
static struct hda_pcm_stream ca0132_pcm_analog_playback = {
	.substreams = 1,
	.channels_min = 2,
	.channels_max = 6,
	.ops = {
		.prepare = ca0132_playback_pcm_prepare,
		.cleanup = ca0132_playback_pcm_cleanup
	},
};

static struct hda_pcm_stream ca0132_pcm_analog_capture = {
	.substreams = 1,
	.channels_min = 2,
	.channels_max = 2,
	.ops = {
		.prepare = ca0132_capture_pcm_prepare,
		.cleanup = ca0132_capture_pcm_cleanup
	},
};

static int ca0132_build_pcms(struct hda_codec *codec)
{
	struct ca0132_spec *spec = codec->spec;
	struct hda_pcm *info = spec->pcm_rec;

	codec->pcm_info = info;
	codec->num_pcms = 0;

	info->name = "CA0132 Analog";
	info->stream[SNDRV_PCM_STREAM_PLAYBACK] = ca0132_pcm_analog_playback;
	info->stream[SNDRV_PCM_STREAM_PLAYBACK].nid = spec->dacs[0];
	info->stream[SNDRV_PCM_STREAM_PLAYBACK].channels_max =
						spec->multiout.max_channels;
	info->stream[SNDRV_PCM_STREAM_CAPTURE] = ca0132_pcm_analog_capture;
	info->stream[SNDRV_PCM_STREAM_CAPTURE].substreams = 1;
	info->stream[SNDRV_PCM_STREAM_CAPTURE].nid = spec->adcs[0];
	codec->num_pcms++;

	info++;
	info->name = "CA0132 Analog Mic-In2";
	info->stream[SNDRV_PCM_STREAM_CAPTURE] = ca0132_pcm_analog_capture;
	info->stream[SNDRV_PCM_STREAM_CAPTURE].substreams = 1;
	info->stream[SNDRV_PCM_STREAM_CAPTURE].nid = spec->adcs[1];
	codec->num_pcms++;

	info++;
	info->name = "CA0132 What U Hear";
	info->stream[SNDRV_PCM_STREAM_CAPTURE] = ca0132_pcm_analog_capture;
	info->stream[SNDRV_PCM_STREAM_CAPTURE].substreams = 1;
	info->stream[SNDRV_PCM_STREAM_CAPTURE].nid = spec->adcs[2];
	codec->num_pcms++;

	return 0;
}

static void init_output(struct hda_codec *codec, hda_nid_t pin, hda_nid_t dac)
{
	if (pin) {
		snd_hda_codec_write(codec, pin, 0,
				    AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_HP);
		if (get_wcaps(codec, pin) & AC_WCAP_OUT_AMP)
			snd_hda_codec_write(codec, pin, 0,
					    AC_VERB_SET_AMP_GAIN_MUTE,
					    AMP_OUT_UNMUTE);
	}
	if (dac)
		snd_hda_codec_write(codec, dac, 0, AC_VERB_SET_AMP_GAIN_MUTE,
				    AMP_OUT_ZERO);
}

static void init_input(struct hda_codec *codec, hda_nid_t pin, hda_nid_t adc)
{
	if (pin) {
		snd_hda_codec_write(codec, pin, 0,
				    AC_VERB_SET_PIN_WIDGET_CONTROL,
				    PIN_VREF80);
		if (get_wcaps(codec, pin) & AC_WCAP_IN_AMP)
			snd_hda_codec_write(codec, pin, 0,
					    AC_VERB_SET_AMP_GAIN_MUTE,
					    AMP_IN_UNMUTE(0));
	}
	if (adc) {
		snd_hda_codec_write(codec, adc, 0, AC_VERB_SET_AMP_GAIN_MUTE,
				    AMP_IN_UNMUTE(0));

		/* init to 0 dB and unmute. */
		snd_hda_codec_amp_stereo(codec, adc, HDA_INPUT, 0,
					 HDA_AMP_VOLMASK, 0x5a);
		snd_hda_codec_amp_stereo(codec, adc, HDA_INPUT, 0,
					 HDA_AMP_MUTE, 0);
	}
}

static void ca0132_init_unsol(struct hda_codec *codec)
{
	snd_hda_jack_detect_enable(codec, UNSOL_TAG_HP, UNSOL_TAG_HP);
	snd_hda_jack_detect_enable(codec, UNSOL_TAG_AMIC1, UNSOL_TAG_AMIC1);
}

static void refresh_amp_caps(struct hda_codec *codec, hda_nid_t nid, int dir)
{
	unsigned int caps;

	caps = snd_hda_param_read(codec, nid, dir == HDA_OUTPUT ?
				  AC_PAR_AMP_OUT_CAP : AC_PAR_AMP_IN_CAP);
	snd_hda_override_amp_caps(codec, nid, dir, caps);
}

static void ca0132_set_dmic_gain(struct hda_codec *codec, unsigned int gain)
{
	unsigned int mic_gain_reg;
	chipio_read(codec, 0x18B098, &mic_gain_reg);
	mic_gain_reg = (mic_gain_reg & ~0x1F) | gain;
	chipio_write(codec, 0x18B098, mic_gain_reg);

	chipio_read(codec, 0x18B09C, &mic_gain_reg);
	mic_gain_reg = (mic_gain_reg & ~0x1F) | gain;
	chipio_write(codec, 0x18B09C, mic_gain_reg);
}

static void ca0132_set_dmic(struct hda_codec *codec, int enable)
{
	struct ca0132_spec *spec = codec->spec;
	unsigned int tmp;
	u8 val;
	unsigned int oldval;

	CA0132_LOG("ca0132_set_dmic: enable=%d\n", enable);

	oldval = stop_mic1(codec);
	ca0132_set_vipsource(codec, 0);
	if (enable) {
		/* set DMic input as 2-ch */
		tmp = FLOAT_TWO;
		dspio_set_param(codec, 0x80, 0x00, &tmp, sizeof(unsigned int));

		val = spec->dmic_ctl;
		val |= 0x80;
		snd_hda_codec_write(codec, spec->input_pins[0], 0,
				    VENDOR_CHIPIO_DMIC_CTL_SET, val);

		if (!(spec->dmic_ctl & 0x20))
			chipio_set_control_flag(codec, CONTROL_FLAG_DMIC, 1);

		ca0132_set_dmic_gain(codec, 0x14);
	} else {
		/* set AMic input as mono */
		tmp = FLOAT_ONE;
		dspio_set_param(codec, 0x80, 0x00, &tmp, sizeof(unsigned int));

		val = spec->dmic_ctl;
		/* clear bit7 and bit5 to disable dmic */
		val &= 0x5f;
		snd_hda_codec_write(codec, spec->input_pins[0], 0,
				    VENDOR_CHIPIO_DMIC_CTL_SET, val);

		if (!(spec->dmic_ctl & 0x20))
			chipio_set_control_flag(codec, CONTROL_FLAG_DMIC, 0);

		ca0132_set_dmic_gain(codec, 0x0);
	}
	ca0132_set_vipsource(codec, 1);
	resume_mic1(codec, oldval);
}

static void ca0132_init_dmic(struct hda_codec *codec)
{
	struct ca0132_spec *spec = codec->spec;
	u8 val;

	/* Setup Digital Mic here, but don't enable.
	 * Enable based on jack detect.
	 */

	/* MCLK uses MPIO1, set to enable.
	 * Bit 2-0: MPIO select
	 * Bit   3: set to disable
	 * Bit 7-4: reserved
	 */
	val = 0x01;
	snd_hda_codec_write(codec, spec->input_pins[0], 0,
			    VENDOR_CHIPIO_DMIC_MCLK_SET, val);

	/* Data1 uses MPIO3. Data2 not use
	 * Bit 2-0: Data1 MPIO select
	 * Bit   3: set disable Data1
	 * Bit 6-4: Data2 MPIO select
	 * Bit   7: set disable Data2
	 */
	val = 0x83;
	snd_hda_codec_write(codec, spec->input_pins[0], 0,
			    VENDOR_CHIPIO_DMIC_PIN_SET, val);

	/* Use Ch-0 and Ch-1. Rate is 48K, mode 1. Disable DMic first.
	 * Bit 3-0: Channel mask
	 * Bit   4: set for 48KHz, clear for 32KHz
	 * Bit   5: mode
	 * Bit   6: set to select Data2, clear for Data1
	 * Bit   7: set to enable DMic, clear for AMic
	 */
	val = 0x23;
	/* keep a copy of dmic ctl val for enable/disable dmic purpuse */
	spec->dmic_ctl = val;
	snd_hda_codec_write(codec, spec->input_pins[0], 0,
			    VENDOR_CHIPIO_DMIC_CTL_SET, val);
}

static void ca0132_init_analog_mic2(struct hda_codec *codec)
{
	struct ca0132_spec *spec = codec->spec;

	mutex_lock(&spec->chipio_mutex);
	snd_hda_codec_write(codec, WIDGET_CHIP_CTRL, 0,
			    VENDOR_CHIPIO_8051_ADDRESS_LOW, 0x20);
	snd_hda_codec_write(codec, WIDGET_CHIP_CTRL, 0,
			    VENDOR_CHIPIO_8051_ADDRESS_HIGH, 0x19);
	snd_hda_codec_write(codec, WIDGET_CHIP_CTRL, 0,
			    VENDOR_CHIPIO_8051_DATA_WRITE, 0x00);
	snd_hda_codec_write(codec, WIDGET_CHIP_CTRL, 0,
			    VENDOR_CHIPIO_8051_ADDRESS_LOW, 0x2D);
	snd_hda_codec_write(codec, WIDGET_CHIP_CTRL, 0,
			    VENDOR_CHIPIO_8051_ADDRESS_HIGH, 0x19);
	snd_hda_codec_write(codec, WIDGET_CHIP_CTRL, 0,
			    VENDOR_CHIPIO_8051_DATA_WRITE, 0x00);
	mutex_unlock(&spec->chipio_mutex);
}

static void ca0132_refresh_widget_caps(struct hda_codec *codec)
{
	struct ca0132_spec *spec = codec->spec;
	int i;
	hda_nid_t nid;

	CA0132_LOG("ca0132_refresh_widget_caps.\n");
	nid = codec->start_nid;
	for (i = 0; i < codec->num_nodes; i++, nid++)
		codec->wcaps[i] = snd_hda_param_read(codec, nid,
						     AC_PAR_AUDIO_WIDGET_CAP);

	for (i = 0; i < spec->multiout.num_dacs; i++)
		refresh_amp_caps(codec, spec->dacs[i], HDA_OUTPUT);

	for (i = 0; i < spec->num_outputs; i++)
		refresh_amp_caps(codec, spec->out_pins[i], HDA_OUTPUT);

	for (i = 0; i < spec->num_inputs; i++) {
		refresh_amp_caps(codec, spec->adcs[i], HDA_INPUT);
		refresh_amp_caps(codec, spec->input_pins[i], HDA_INPUT);
	}
}

static void ca0132_setup_defaults(struct hda_codec *codec)
{
	unsigned int tmp;
	int num_fx;
	int idx, i;

	if (!dspload_is_loaded(codec))
		return;

	/* out, in effects + voicefx */
	num_fx = OUT_EFFECTS_COUNT + IN_EFFECTS_COUNT + 1;
	for (idx = 0; idx < num_fx; idx++) {
		for (i = 0; i <= ca0132_effects[idx].params; i++) {
			dspio_set_param(codec, ca0132_effects[idx].mid,
					ca0132_effects[idx].reqs[i],
					&(ca0132_effects[idx].def_vals[i]),
					sizeof(unsigned int));
		}
	}

	/*remove DSP headroom*/
	tmp = FLOAT_ZERO;
	dspio_set_param(codec, 0x96, 0x3C, &tmp, sizeof(unsigned int));

	/*set speaker EQ bypass attenuation*/
	dspio_set_param(codec, 0x8f, 0x01, &tmp, sizeof(unsigned int));

	/* set AMic1 and AMic2 as mono mic */
	tmp = FLOAT_ONE;
	dspio_set_param(codec, 0x80, 0x00, &tmp, sizeof(unsigned int));
	dspio_set_param(codec, 0x80, 0x01, &tmp, sizeof(unsigned int));

	/* set AMic1 as CrystalVoice input */
	tmp = FLOAT_ONE;
	dspio_set_param(codec, 0x80, 0x05, &tmp, sizeof(unsigned int));

	/* set WUH source */
	tmp = FLOAT_TWO;
	dspio_set_param(codec, 0x31, 0x00, &tmp, sizeof(unsigned int));

	/* set AEC level */
	tmp = FLOAT_60;
	dspio_set_param(codec, 0x86, 0x05, &tmp, sizeof(unsigned int));
}

static void ca0132_init_flags(struct hda_codec *codec)
{
	chipio_set_control_flag(codec, CONTROL_FLAG_IDLE_ENABLE, 0);
	chipio_set_control_flag(codec, CONTROL_FLAG_PORT_A_COMMON_MODE, 0);
	chipio_set_control_flag(codec, CONTROL_FLAG_PORT_D_COMMON_MODE, 0);
	chipio_set_control_flag(codec, CONTROL_FLAG_PORT_A_10KOHM_LOAD, 0);
	chipio_set_control_flag(codec, CONTROL_FLAG_PORT_D_10KOHM_LOAD, 0);
	chipio_set_control_flag(codec, CONTROL_FLAG_ADC_C_HIGH_PASS, 1);
}

static void ca0132_init_params(struct hda_codec *codec)
{
	chipio_set_control_param(codec, CONTROL_PARAM_PORTA_160OHM_GAIN, 6);
	chipio_set_control_param(codec, CONTROL_PARAM_PORTD_160OHM_GAIN, 6);
}

static void ca0132_set_dsp_msr(struct hda_codec *codec, bool is96k)
{
	chipio_set_control_flag(codec, CONTROL_FLAG_DSP_96KHZ, is96k);
	chipio_set_control_flag(codec, CONTROL_FLAG_DAC_96KHZ, is96k);
	chipio_set_control_flag(codec, CONTROL_FLAG_SRC_RATE_96KHZ, is96k);
	chipio_set_control_flag(codec, CONTROL_FLAG_SRC_CLOCK_196MHZ, is96k);
	chipio_set_control_flag(codec, CONTROL_FLAG_ADC_B_96KHZ, is96k);
	chipio_set_control_flag(codec, CONTROL_FLAG_ADC_C_96KHZ, is96k);

	chipio_set_conn_rate(codec, MEM_CONNID_MICIN1, SR_96_000);
	chipio_set_conn_rate(codec, MEM_CONNID_MICOUT1, SR_96_000);
	chipio_set_conn_rate(codec, MEM_CONNID_WUH, SR_48_000);
}

static bool ca0132_download_dsp_images(struct hda_codec *codec)
{
	bool dsp_loaded = false;
	const struct dsp_image_seg *dsp_os_image;

	if (request_firmware_cached(&fw_efx, EFX_FILE,
				    codec->bus->card->dev) != 0)
		return false;

	dsp_os_image = (struct dsp_image_seg *)(fw_efx->data);
	dspload_image(codec, dsp_os_image, 0, 0, true, 0);
	dsp_loaded = dspload_wait_loaded(codec);

	if (dsp_loaded)
		dspload_speakereq(codec);

	return dsp_loaded;
}

static void ca0132_download_dsp(struct hda_codec *codec)
{
	struct ca0132_spec *spec = codec->spec;

	spec->dsp_state = DSP_DOWNLOAD_INIT;

	if (spec->dsp_state == DSP_DOWNLOAD_INIT) {
		chipio_enable_clocks(codec);
		spec->dsp_state = DSP_DOWNLOADING;
		if (!ca0132_download_dsp_images(codec))
			spec->dsp_state = DSP_DOWNLOAD_FAILED;
		else
			spec->dsp_state = DSP_DOWNLOADED;
	}

	if (spec->dsp_state == DSP_DOWNLOADED)
		ca0132_set_dsp_msr(codec, true);
}

static void ca0132_process_dsp_response(struct hda_codec *codec)
{
	struct ca0132_spec *spec = codec->spec;

	CA0132_LOG("ca0132_process_dsp_response\n");
	if (spec->wait_scp) {
		if (dspio_get_response_data(codec) >= 0)
			spec->wait_scp = 0;
	}

	dspio_clear_response_queue(codec);
}

static void ca0132_unsol_event(struct hda_codec *codec, unsigned int res)
{
	struct ca0132_spec *spec = codec->spec;
	CA0132_LOG("ca0132_unsol_event: 0x%x\n", res);


	if (((res >> AC_UNSOL_RES_TAG_SHIFT) & 0x3f) == UNSOL_TAG_DSP) {
		ca0132_process_dsp_response(codec);
	} else {
		res = snd_hda_jack_get_action(codec,
				(res >> AC_UNSOL_RES_TAG_SHIFT) & 0x3f);

		CA0132_LOG("snd_hda_jack_get_action: 0x%x\n", res);

		switch (res) {
		case UNSOL_TAG_HP:
			cancel_delayed_work_sync(&spec->unsol_hp_work);
			queue_delayed_work(codec->bus->workq,
					   &spec->unsol_hp_work,
					   msecs_to_jiffies(250));
			break;
		case UNSOL_TAG_AMIC1:
			ca0132_select_mic(codec);
			snd_hda_jack_report_sync(codec);
			break;
		default:
			break;
		}
	}
}

/*
 * Verbs tables.
 */

/* Sends before DSP download. */
static struct hda_verb ca0132_base_init_verbs[] = {
	/*enable ct extension*/
	{0x15, VENDOR_CHIPIO_CT_EXTENSIONS_ENABLE, 0x1},
	/*enable DSP node unsol, needed for DSP download*/
	{0x16, AC_VERB_SET_UNSOLICITED_ENABLE, AC_USRSP_EN | UNSOL_TAG_DSP},
	{}
};

/* Send at exit. */
static struct hda_verb ca0132_base_exit_verbs[] = {
	/*set afg to D3*/
	{0x01, AC_VERB_SET_POWER_STATE, 0x03},
	/*disable ct extension*/
	{0x15, VENDOR_CHIPIO_CT_EXTENSIONS_ENABLE, 0},
	{}
};

/* Other verbs tables.  Sends after DSP download. */
static struct hda_verb ca0132_init_verbs0[] = {
	/* chip init verbs */
	{0x15, 0x70D, 0xF0},
	{0x15, 0x70E, 0xFE},
	{0x15, 0x707, 0x75},
	{0x15, 0x707, 0xD3},
	{0x15, 0x707, 0x09},
	{0x15, 0x707, 0x53},
	{0x15, 0x707, 0xD4},
	{0x15, 0x707, 0xEF},
	{0x15, 0x707, 0x75},
	{0x15, 0x707, 0xD3},
	{0x15, 0x707, 0x09},
	{0x15, 0x707, 0x02},
	{0x15, 0x707, 0x37},
	{0x15, 0x707, 0x78},
	{0x15, 0x53C, 0xCE},
	{0x15, 0x575, 0xC9},
	{0x15, 0x53D, 0xCE},
	{0x15, 0x5B7, 0xC9},
	{0x15, 0x70D, 0xE8},
	{0x15, 0x70E, 0xFE},
	{0x15, 0x707, 0x02},
	{0x15, 0x707, 0x68},
	{0x15, 0x707, 0x62},
	{0x15, 0x53A, 0xCE},
	{0x15, 0x546, 0xC9},
	{0x15, 0x53B, 0xCE},
	{0x15, 0x5E8, 0xC9},
	{0x15, 0x717, 0x0D},
	{0x15, 0x718, 0x20},
	{}
};

static struct hda_verb ca0132_init_verbs1[] = {
	{0x10, AC_VERB_SET_UNSOLICITED_ENABLE, AC_USRSP_EN | UNSOL_TAG_HP},
	{0x12, AC_VERB_SET_UNSOLICITED_ENABLE, AC_USRSP_EN | UNSOL_TAG_AMIC1},
	/* Set the default configuration for the mic pin because the
	 * value set in BIOS will be reset after ct extension is enabled.
	 */
	{0x12, AC_VERB_SET_CONFIG_DEFAULT_BYTES_0, 0xf0},
	{0x12, AC_VERB_SET_CONFIG_DEFAULT_BYTES_1, 0x10},
	{0x12, AC_VERB_SET_CONFIG_DEFAULT_BYTES_2, 0xa1},
	{0x12, AC_VERB_SET_CONFIG_DEFAULT_BYTES_3, 0x03},
	/* config EAPD */
	{0x0b, 0x78D, 0x00},
	/*{0x0b, AC_VERB_SET_EAPD_BTLENABLE, 0x02},*/
	/*{0x10, 0x78D, 0x02},*/
	/*{0x10, AC_VERB_SET_EAPD_BTLENABLE, 0x02},*/
	{}
};

static void ca0132_init_chip(struct hda_codec *codec)
{
	struct ca0132_spec *spec = codec->spec;
	int num_fx;
	int i;
	unsigned int on;

	mutex_init(&spec->chipio_mutex);

	spec->cur_out_type = SPEAKER_OUT;
	spec->cur_mic_type = DIGITAL_MIC;
	spec->cur_mic_boost = 0;

	for (i = 0; i < VNODES_COUNT; i++) {
		spec->vnode_lvol[i] = 0x5a;
		spec->vnode_rvol[i] = 0x5a;
		spec->vnode_lswitch[i] = 0;
		spec->vnode_rswitch[i] = 0;
	}

	/*
	 * Default states for effects are in ca0132_effects[].
	 */
	num_fx = OUT_EFFECTS_COUNT + IN_EFFECTS_COUNT;
	for (i = 0; i < num_fx; i++) {
		on = (unsigned int)ca0132_effects[i].reqs[0];
		spec->effects_switch[i] = on ? 1 : 0;
	}

	spec->voicefx_val = 0;
	spec->effects_switch[PLAY_ENHANCEMENT - EFFECT_START_NID] = 1;
	spec->effects_switch[CRYSTAL_VOICE - EFFECT_START_NID] = 0;

	#ifdef ENABLE_TUNING_CONTROLS
	ca0132_init_tuning_defaults(codec);
	#endif
}

static void ca0132_exit_chip(struct hda_codec *codec)
{
	/* put any chip cleanup stuffs here. */

	if (dspload_is_loaded(codec))
		dsp_reset(codec);
}

static int xdata_read(struct hda_codec *codec, u16 addr)
{
	struct ca0132_spec *spec = codec->spec;
	int data;

	mutex_lock(&spec->chipio_mutex);
	snd_hda_codec_write(codec, WIDGET_CHIP_CTRL, 0,
			    VENDOR_CHIPIO_8051_ADDRESS_LOW, addr & 0xFF);
	snd_hda_codec_write(codec, WIDGET_CHIP_CTRL, 0,
			    VENDOR_CHIPIO_8051_ADDRESS_HIGH,
			    (addr >> 8) & 0xFF);
	data = snd_hda_codec_read(codec, WIDGET_CHIP_CTRL, 0,
			    VENDOR_CHIPIO_8051_DATA_READ, 0);
	mutex_unlock(&spec->chipio_mutex);

	return data;
}

static int xdata_write(struct hda_codec *codec, u16 addr, u8 value)
{
	struct ca0132_spec *spec = codec->spec;

	mutex_lock(&spec->chipio_mutex);
	snd_hda_codec_write(codec, WIDGET_CHIP_CTRL, 0,
			    VENDOR_CHIPIO_8051_ADDRESS_LOW, addr & 0xFF);
	snd_hda_codec_write(codec, WIDGET_CHIP_CTRL, 0,
			    VENDOR_CHIPIO_8051_ADDRESS_HIGH,
			    (addr >> 8) & 0xFF);
	snd_hda_codec_write(codec, WIDGET_CHIP_CTRL, 0,
			    VENDOR_CHIPIO_8051_DATA_WRITE, value);
	mutex_unlock(&spec->chipio_mutex);

	return 0;
}

static int idata_write(struct hda_codec *codec, u16 addr, u8 value)
{
	struct ca0132_spec *spec = codec->spec;

	mutex_lock(&spec->chipio_mutex);
	snd_hda_codec_write(codec, WIDGET_CHIP_CTRL, 0,
			    VENDOR_CHIPIO_8051_ADDRESS_LOW, addr & 0xFF);
	snd_hda_codec_write(codec, WIDGET_CHIP_CTRL, 0,
			    VENDOR_CHIPIO_8051_ADDRESS_HIGH,
			    (addr >> 8) & 0xFF);
	snd_hda_codec_write(codec, WIDGET_CHIP_CTRL, 0,
			    0x709, value);
	mutex_unlock(&spec->chipio_mutex);

	return 0;
}

static int pmu_read(struct hda_codec *codec, u8 addr)
{
	struct ca0132_spec *spec = codec->spec;
	int data;

	mutex_lock(&spec->chipio_mutex);
	snd_hda_codec_write(codec, WIDGET_CHIP_CTRL, 0,
			    VENDOR_CHIPIO_8051_ADDRESS_LOW, addr);
	data = snd_hda_codec_read(codec, WIDGET_CHIP_CTRL, 0,
			    VENDOR_CHIPIO_PLL_PMU_READ, 0);
	mutex_unlock(&spec->chipio_mutex);

	return data;
}

static int pmu_write(struct hda_codec *codec, u8 addr, u8 value)
{
	struct ca0132_spec *spec = codec->spec;

	mutex_lock(&spec->chipio_mutex);
	snd_hda_codec_write(codec, WIDGET_CHIP_CTRL, 0,
			    VENDOR_CHIPIO_8051_ADDRESS_LOW, addr);
	snd_hda_codec_write(codec, WIDGET_CHIP_CTRL, 0,
			    VENDOR_CHIPIO_PLL_PMU_WRITE, value);
	mutex_unlock(&spec->chipio_mutex);

	return 0;
}

static int ca0132_init_codec(struct hda_codec *codec)
{
	u8 codecDevices[] = {
		1, 2, 3, 6, 8, 10, 11
	};
	bool codec_ready = true;
	int retryCount = 3;
	int tryCount = 5;
	int i;
	int codecStateAddr = 0x18BE;
	int addr;
	int data;

	do {
		codec_ready = true;
		for (i = 0; i < ARRAY_SIZE(codecDevices); i++) {
			addr = codecStateAddr + 13 * codecDevices[i] + 8;
			data = xdata_read(codec, addr);
			if ((data > 0) && (data & 0x4))
				codec_ready = false;
		}

		if (!codec_ready) {
			snd_hda_codec_write(codec, 1, 0,
					    AC_VERB_SET_POWER_STATE, 4);

			data = pmu_read(codec, 0xF);
			pmu_write(codec, 0xF, data & ~0x88);
			msleep(100);
			pmu_write(codec, 0xF, data | 0x88);
			msleep(100);
			xdata_write(codec, 0xB6, 0x1);
			xdata_write(codec, 0x1F07, 0xF9);
			xdata_write(codec, 0x1F08, 0x72);
			idata_write(codec, 0x7D, 0x40);

			do {
				data = xdata_read(codec, 0xB6);
				msleep(100);
			} while (data != 0 && --tryCount);

			snd_hda_codec_write(codec, 1, 0,
					    AC_VERB_SET_POWER_STATE, 0);
			msleep(500);
		}
	} while (!codec_ready && --retryCount);

	return 0;
}

static int ca0132_init(struct hda_codec *codec)
{
	struct ca0132_spec *spec = codec->spec;
	int i;

	spec->dsp_state = DSP_DOWNLOAD_INIT;
	spec->curr_chip_addx = (unsigned int)INVALID_CHIP_ADDRESS;

	snd_hda_power_up(codec);

	snd_hda_sequence_write(codec, spec->base_init_verbs);
	ca0132_init_codec(codec);
	ca0132_init_params(codec);
	ca0132_init_flags(codec);
	ca0132_download_dsp(codec);
	ca0132_refresh_widget_caps(codec);
	ca0132_setup_defaults(codec);
	ca0132_pe_switch_set(codec);
	ca0132_cvoice_switch_set(codec);
	ca0132_init_analog_mic2(codec);
	ca0132_init_dmic(codec);

	for (i = 0; i < spec->num_outputs; i++)
		init_output(codec, spec->out_pins[i], spec->dacs[0]);

	for (i = 0; i < spec->num_inputs; i++)
		init_input(codec, spec->input_pins[i], spec->adcs[i]);

	for (i = 0; i < spec->num_init_verbs; i++)
		snd_hda_sequence_write(codec, spec->init_verbs[i]);

	ca0132_init_unsol(codec);

	ca0132_select_out(codec);
	ca0132_select_mic(codec);

	snd_hda_jack_report_sync(codec);

	snd_hda_power_down(codec);

	return 0;
}

static void ca0132_free(struct hda_codec *codec)
{
	struct ca0132_spec *spec = codec->spec;

	cancel_delayed_work_sync(&spec->unsol_hp_work);
	snd_hda_power_up(codec);
	snd_hda_sequence_write(codec, spec->base_exit_verbs);
	ca0132_exit_chip(codec);
	snd_hda_power_down(codec);
	kfree(codec->spec);
}


static struct hda_codec_ops ca0132_patch_ops = {
	.build_controls = ca0132_build_controls,
	.build_pcms = ca0132_build_pcms,
	.init = ca0132_init,
	.free = ca0132_free,
	.unsol_event = ca0132_unsol_event,
};

static void ca0132_config(struct hda_codec *codec)
{
	struct ca0132_spec *spec = codec->spec;

	spec->dacs[0] = 0x2;
	spec->dacs[1] = 0x3;
	spec->dacs[2] = 0x4;

	spec->multiout.dac_nids = spec->dacs;
	spec->multiout.num_dacs = 3;
	spec->multiout.max_channels = 2;

	spec->num_outputs = 2;
	spec->out_pins[0] = 0x0b; /* speaker out */
	spec->out_pins[1] = 0x10; /* headphone out */
	spec->shared_out_nid = 0x2;

	spec->num_inputs = 3;
	spec->adcs[0] = 0x7; /* digital mic / analog mic1 */
	spec->adcs[1] = 0x8; /* analog mic2 */
	spec->adcs[2] = 0xa; /* what u hear */
	spec->shared_mic_nid = 0x7;

	spec->input_pins[0] = 0x12;
	spec->input_pins[1] = 0x11;
	spec->input_pins[2] = 0x13;
}

static int patch_ca0132(struct hda_codec *codec)
{
	struct ca0132_spec *spec;
	int err;

	snd_printdd("patch_ca0132\n");

	spec = kzalloc(sizeof(*spec), GFP_KERNEL);
	if (!spec)
		return -ENOMEM;
	codec->spec = spec;
	spec->codec = codec;

	spec->num_mixers = 1;
	spec->mixers[0] = ca0132_mixer;

	spec->base_init_verbs = ca0132_base_init_verbs;
	spec->base_exit_verbs = ca0132_base_exit_verbs;
	spec->init_verbs[0] = ca0132_init_verbs0;
	spec->init_verbs[1] = ca0132_init_verbs1;
	spec->num_init_verbs = 2;

	INIT_DELAYED_WORK(&spec->unsol_hp_work, ca0132_unsol_hp_delayed);

	ca0132_init_chip(codec);

	ca0132_config(codec);

	err = snd_hda_parse_pin_def_config(codec, &spec->autocfg, NULL);
	if (err < 0)
		return err;

	codec->patch_ops = ca0132_patch_ops;

	return 0;
}


/*
 * patch entries
 */
static struct hda_codec_preset snd_hda_preset_ca0132[] = {
	{ .id = 0x11020011, .name = "CA0132", .patch = patch_ca0132 },
	{} /* terminator */
};

MODULE_ALIAS("snd-hda-codec-id:11020011");

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Creative Sound Core3D codec");

static struct hda_codec_preset_list ca0132_list = {
	.preset = snd_hda_preset_ca0132,
	.owner = THIS_MODULE,
};

static int __init patch_ca0132_init(void)
{
	return snd_hda_add_codec_preset(&ca0132_list);
}

static void __exit patch_ca0132_exit(void)
{
	release_cached_firmware();
	snd_hda_delete_codec_preset(&ca0132_list);
}

module_init(patch_ca0132_init)
module_exit(patch_ca0132_exit)

/* sound/soc/samsung/abox/abox_dump.c
 *
 * ALSA SoC Audio Layer - Samsung Abox Internal Buffer Dumping driver
 *
 * Copyright (c) 2016 Samsung Electronics Co. Ltd.
  *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
/* #define DEBUG */
#include <linux/module.h>
#include <linux/debugfs.h>
#include <linux/spinlock.h>
#include <linux/vmalloc.h>
#include <sound/samsung/abox.h>

#include "abox_util.h"
#include "abox.h"
#include "abox_dbg.h"
#include "abox_log.h"

#define BUFFER_MAX (SZ_32)
#define NAME_LENGTH (SZ_32)

struct abox_dump_buffer_info {
	struct device *dev;
	struct list_head list;
	int id;
	char name[NAME_LENGTH];
	struct mutex lock;
	struct snd_dma_buffer buffer;
	struct snd_pcm_substream *substream;
	size_t pointer;
	bool started;
	bool auto_started;
	bool file_created;
	struct file *filp;
	ssize_t auto_pointer;
	struct work_struct auto_work;
};

static struct device *abox_dump_dev_abox __read_mostly;
static LIST_HEAD(abox_dump_list_head);

static struct abox_dump_buffer_info *abox_dump_get_buffer_info(int id)
{
	struct abox_dump_buffer_info *info;

	list_for_each_entry(info, &abox_dump_list_head, list) {
		if (info->id == id) {
			return info;
		}
	}

	return NULL;
}

static struct abox_dump_buffer_info *abox_dump_get_buffer_info_by_name(const char *name)
{
	struct abox_dump_buffer_info *info;

	list_for_each_entry(info, &abox_dump_list_head, list) {
		if (strncmp(info->name, name, sizeof(info->name)) == 0) {
			return info;
		}
	}

	return NULL;
}

static void abox_dump_request_dump(int id)
{
	struct abox_dump_buffer_info *info = abox_dump_get_buffer_info(id);
	ABOX_IPC_MSG msg;
	struct IPC_SYSTEM_MSG *system = &msg.msg.system;
	bool start = info->started || info->auto_started;

	dev_dbg(abox_dump_dev_abox, "%s(%d)\n", __func__, id);

	msg.ipcid = IPC_SYSTEM;
	system->msgtype = ABOX_REQUEST_DUMP;
	system->param1 = id;
	system->param2 = start ? 1 : 0;
	abox_start_ipc_transaction(abox_dump_dev_abox, msg.ipcid, &msg, sizeof(msg), 0, 0);
}

static ssize_t abox_dump_auto_read(struct file *file, char __user *data, size_t count, loff_t *ppos, bool enable)
{
	struct abox_dump_buffer_info *info;
	char buffer[SZ_256] = {0,}, *buffer_p = buffer;

	dev_dbg(abox_dump_dev_abox, "%s(%zu, %lld, %d)\n", __func__, count, *ppos, enable);

	list_for_each_entry(info, &abox_dump_list_head, list) {
		if (info->auto_started == enable) {
			buffer_p += snprintf(buffer_p, sizeof(buffer) - (buffer_p - buffer),
					"%d(%s) ", info->id, info->name);
		}
	}
	snprintf(buffer_p, 2, "\n");

	return simple_read_from_buffer(data, count, ppos, buffer, buffer_p - buffer);
}

static ssize_t abox_dump_auto_write(struct file *file, const char __user *data, size_t count, loff_t *ppos, bool enable)
{
	char buffer[SZ_256] = {0,}, name[NAME_LENGTH];
	char *p_buffer = buffer, *token = NULL;
	unsigned int id;
	struct abox_dump_buffer_info *info;

	dev_dbg(abox_dump_dev_abox, "%s(%zu, %lld, %d)\n", __func__, count, *ppos, enable);

	simple_write_to_buffer(buffer, sizeof(buffer), ppos, data, count);

	while ((token = strsep(&p_buffer, " ")) != NULL) {
		if (sscanf(token, "%11u", &id) == 1) {
			info = abox_dump_get_buffer_info(id);
		} else if (sscanf(token, "%31s", name) == 1) {
			info = abox_dump_get_buffer_info_by_name(name);
		} else {
			info = NULL;
		}

		if (IS_ERR_OR_NULL(info)) {
			dev_err(abox_dump_dev_abox, "invalid argument\n");
			continue;
		}

		info->auto_started = enable;
		if (enable) {
			info->file_created = false;
			info->auto_pointer = -1;
		}

		abox_dump_request_dump(info->id);
	}

	return count;
}

static ssize_t abox_dump_auto_start_read(struct file *file, char __user *data, size_t count, loff_t *ppos)
{
	return abox_dump_auto_read(file, data, count, ppos, true);
}

static ssize_t abox_dump_auto_start_write(struct file *file, const char __user *data, size_t count, loff_t *ppos)
{
	return abox_dump_auto_write(file, data, count, ppos, true);
}

static ssize_t abox_dump_auto_stop_read(struct file *file, char __user *data, size_t count, loff_t *ppos)
{
	return abox_dump_auto_read(file, data, count, ppos, false);
}

static ssize_t abox_dump_auto_stop_write(struct file *file, const char __user *data, size_t count, loff_t *ppos)
{
	return abox_dump_auto_write(file, data, count, ppos, false);
}

static struct file_operations abox_dump_auto_start_fops = {
	.read = abox_dump_auto_start_read,
	.write = abox_dump_auto_start_write,
};

static struct file_operations abox_dump_auto_stop_fops = {
	.read = abox_dump_auto_stop_read,
	.write = abox_dump_auto_stop_write,
};

static int __init samsung_abox_dump_late_initcall(void)
{
	pr_info("%s\n", __func__);

	debugfs_create_file("dump_auto_start", S_IRUGO | S_IWUGO, abox_dbg_get_root_dir(), NULL, &abox_dump_auto_start_fops);
	debugfs_create_file("dump_auto_stop", S_IRUGO | S_IWUGO, abox_dbg_get_root_dir(), NULL, &abox_dump_auto_stop_fops);

	return 0;
}
late_initcall(samsung_abox_dump_late_initcall);

static struct snd_soc_dai_link abox_dump_dai_links[BUFFER_MAX];

static struct snd_soc_card abox_dump_card = {
	.name = "abox_dump",
	.owner = THIS_MODULE,
	.dai_link = abox_dump_dai_links,
	.num_links = 0,
};

static void abox_dump_auto_dump_work_func(struct work_struct *work)
{
	struct abox_dump_buffer_info *info = container_of(work, struct abox_dump_buffer_info, auto_work);
	struct device *dev = info->dev;
	int id = info->id;

	if (info->auto_started) {
		mm_segment_t old_fs;
		char filename[SZ_64];
		struct file *filp;

		sprintf(filename, "/data/abox_dump-%d.raw", id);

		old_fs = get_fs();
		set_fs(KERNEL_DS);
		if (likely(info->file_created)) {
			filp = filp_open(filename, O_RDWR | O_APPEND | O_CREAT, S_IRUSR | S_IWUSR);
			dev_dbg(dev, "appended\n");
		} else {
			filp = filp_open(filename, O_RDWR | O_TRUNC | O_CREAT, S_IRUSR | S_IWUSR);
			info->file_created = true;
			dev_dbg(dev, "created\n");
		}
		if (!IS_ERR_OR_NULL(filp)) {
			void *area = info->buffer.area;
			size_t bytes = info->buffer.bytes;
			size_t pointer = info->pointer;
			bool first = false;

			if (unlikely(info->auto_pointer < 0)) {
				info->auto_pointer = pointer;
				first = true;
			}
			/* dev_dbg(dev, "%pad, %p, %zx, %zx)\n", &info->buffer.addr, area, bytes, info->auto_pointer); */
			if (pointer < info->auto_pointer || first) {
				vfs_write(filp, area + info->auto_pointer, bytes - info->auto_pointer, &filp->f_pos);
				/* dev_dbg(dev, "vfs_write(%p, %zx)\n", area + info->auto_pointer, bytes - info->auto_pointer); */
				info->auto_pointer = 0;
			}
			vfs_write(filp, area + info->auto_pointer, pointer - info->auto_pointer, &filp->f_pos);
			/* dev_dbg(dev, "vfs_write(%p, %zx)\n", area + info->auto_pointer, pointer - info->auto_pointer); */
			info->auto_pointer = pointer;

			vfs_fsync(filp, 1);
			filp_close(filp, NULL);
		} else {
			dev_err(dev, "dump file %d open error: %ld\n", id, PTR_ERR(filp));
		}

		set_fs(old_fs);
	}
}

static struct abox_dump_buffer_info abox_dump_buffer_info_new;

void abox_dump_register_buffer_work_func(struct work_struct *work)
{
	struct device *dev;
	int id;
	struct snd_dma_buffer buffer;
	struct abox_dump_buffer_info *info;
	char name[NAME_LENGTH];

	dev = abox_dump_buffer_info_new.dev;
	id = abox_dump_buffer_info_new.id;
	strncpy(name, abox_dump_buffer_info_new.name, sizeof(abox_dump_buffer_info_new.name) - 1);
	buffer = abox_dump_buffer_info_new.buffer;
	abox_dump_buffer_info_new.dev = NULL;
	abox_dump_buffer_info_new.id = 0;
	abox_dump_buffer_info_new.name[0] = '\0';

	dev_info(dev, "%s[%d]\n", __func__, id);

	if (dev == NULL || id < 0) {
		dev_err(dev, "dev(%p) or id(%d) is wrong\n", dev, id);
		return;
	}

	info = vmalloc(sizeof(*info));
	mutex_init(&info->lock);
	info->id = id;
	strncpy(info->name, name, sizeof(info->name) - 1);
	info->dev = dev;
	info->buffer = buffer;
	info->substream = NULL;
	info->started = false;
	info->auto_started = false;
	INIT_WORK(&info->auto_work, abox_dump_auto_dump_work_func);

	list_add_tail(&info->list, &abox_dump_list_head);
	platform_device_register_simple("samsung-abox-dump", id, NULL, 0);
}

static DECLARE_WORK(abox_dump_register_buffer_work, abox_dump_register_buffer_work_func);

int abox_dump_register_buffer(struct device *dev, int id, const char *name, void *area, phys_addr_t addr, size_t bytes)
{
	struct abox_dump_buffer_info *info;

	dev_dbg(dev, "%s[%d] %p(%pa)\n", __func__, id, area, &addr);

	if (abox_dump_buffer_info_new.dev != NULL ||
			abox_dump_buffer_info_new.id > 0) {
		return -EBUSY;
	}

	list_for_each_entry(info, &abox_dump_list_head, list) {
		if (info->id == id) {
			dev_dbg(dev, "already registered dump: %d\n", id);
			return 0;
		}
	}

	abox_dump_dev_abox = abox_dump_buffer_info_new.dev = dev;
	abox_dump_buffer_info_new.id = id;
	strncpy(abox_dump_buffer_info_new.name, name, sizeof(abox_dump_buffer_info_new.name) - 1);
	abox_dump_buffer_info_new.buffer.area = area;
	abox_dump_buffer_info_new.buffer.addr = addr;
	abox_dump_buffer_info_new.buffer.bytes = bytes;
	schedule_work(&abox_dump_register_buffer_work);

	return 0;
}
EXPORT_SYMBOL(abox_dump_register_buffer);

static struct snd_pcm_hardware abox_dump_hardware = {
	.info			= SNDRV_PCM_INFO_INTERLEAVED
				| SNDRV_PCM_INFO_BLOCK_TRANSFER
				| SNDRV_PCM_INFO_MMAP
				| SNDRV_PCM_INFO_MMAP_VALID,
	.formats		= ABOX_SAMPLE_FORMATS,
	.rates			= SNDRV_PCM_RATE_8000_192000 | SNDRV_PCM_RATE_KNOT,
	.rate_min		= 8000,
	.rate_max		= 384000,
	.channels_min		= 1,
	.channels_max		= 8,
	.periods_min		= 2,
	.periods_max		= 2,
};

static int abox_dump_platform_open(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_platform *platform = rtd->platform;
	struct device *dev = platform->dev;
	int id = to_platform_device(dev)->id;
	struct abox_dump_buffer_info *info = abox_dump_get_buffer_info(id);
	struct snd_dma_buffer *dmab = &substream->dma_buffer;

	dev_dbg(dev, "%s[%d]\n", __func__, id);

	abox_dump_hardware.buffer_bytes_max = dmab->bytes;
	abox_dump_hardware.period_bytes_min = dmab->bytes / abox_dump_hardware.periods_max;
	abox_dump_hardware.period_bytes_max = dmab->bytes / abox_dump_hardware.periods_min;

	snd_soc_set_runtime_hwparams(substream, &abox_dump_hardware);

	info->substream = substream;

	return 0;
}

static int abox_dump_platform_close(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_platform *platform = rtd->platform;
	struct device *dev = platform->dev;
	int id = to_platform_device(dev)->id;
	struct abox_dump_buffer_info *info = abox_dump_get_buffer_info(id);

	dev_dbg(dev, "%s[%d]\n", __func__, id);

	info->substream = NULL;

	return 0;
}

static int abox_dump_platform_hw_params(struct snd_pcm_substream *substream, struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_platform *platform = rtd->platform;
	struct device *dev = platform->dev;
	int id = to_platform_device(dev)->id;

	dev_dbg(dev, "%s[%d]\n", __func__, id);

	return snd_pcm_lib_malloc_pages(substream, params_buffer_bytes(params));
}

static int abox_dump_platform_hw_free(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_platform *platform = rtd->platform;
	struct device *dev = platform->dev;
	int id = to_platform_device(dev)->id;

	dev_dbg(dev, "%s[%d]\n", __func__, id);

	return snd_pcm_lib_free_pages(substream);;
}

static int abox_dump_platform_prepare(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_platform *platform = rtd->platform;
	struct device *dev = platform->dev;
	int id = to_platform_device(dev)->id;
	struct abox_dump_buffer_info *info = abox_dump_get_buffer_info(id);

	dev_dbg(dev, "%s[%d]\n", __func__, id);

	info->pointer = 0;

	return 0;
}

static int abox_dump_platform_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_platform *platform = rtd->platform;
	struct device *dev = platform->dev;
	int id = to_platform_device(dev)->id;
	struct abox_dump_buffer_info *info = abox_dump_get_buffer_info(id);

	dev_dbg(dev, "%s[%d](%d)\n", __func__, id, cmd);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		info->started = true;
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		info->started = false;
		break;
	default:
		dev_err(dev, "invalid command: %d\n", cmd);
		return -EINVAL;
	}

	abox_dump_request_dump(id);

	return 0;
}

void abox_dump_period_elapsed(int id, size_t pointer)
{
	struct abox_dump_buffer_info *info = abox_dump_get_buffer_info(id);
	struct device *dev = info->dev;

	dev_dbg(dev, "%s[%d](%zx)\n", __func__, id, pointer);

	info->pointer = pointer;
	schedule_work(&info->auto_work);
	snd_pcm_period_elapsed(info->substream);
}
EXPORT_SYMBOL(abox_dump_period_elapsed);

static snd_pcm_uframes_t abox_dump_platform_pointer(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_platform *platform = rtd->platform;
	struct device *dev = platform->dev;
	int id = to_platform_device(dev)->id;
	struct abox_dump_buffer_info *info = abox_dump_get_buffer_info(id);

	dev_dbg(dev, "%s[%d]\n", __func__, id);

	return bytes_to_frames(substream->runtime, info->pointer);
}

static struct snd_pcm_ops abox_dump_platform_ops = {
	.open		= abox_dump_platform_open,
	.close		= abox_dump_platform_close,
	.ioctl		= snd_pcm_lib_ioctl,
	.hw_params	= abox_dump_platform_hw_params,
	.hw_free	= abox_dump_platform_hw_free,
	.prepare	= abox_dump_platform_prepare,
	.trigger	= abox_dump_platform_trigger,
	.pointer	= abox_dump_platform_pointer,
};

static void abox_dump_register_card_work_func(struct work_struct *work)
{
	pr_debug("%s\n", __func__);

	snd_soc_unregister_card(&abox_dump_card);
	snd_soc_register_card(&abox_dump_card);
}

DECLARE_DELAYED_WORK(abox_dump_register_card_work, abox_dump_register_card_work_func);

static void abox_dump_add_dai_link(struct device *dev)
{
	int id = to_platform_device(dev)->id;
	struct abox_dump_buffer_info *info = abox_dump_get_buffer_info(id);
	struct snd_soc_dai_link *link = &abox_dump_dai_links[abox_dump_card.num_links++];
	char stream_name[NAME_LENGTH];

	dev_dbg(dev, "%s[%d]\n", __func__, id);

	if (abox_dump_card.num_links > ARRAY_SIZE(abox_dump_dai_links)) {
		dev_err(dev, "Too many dump request\n");
		return;
	}

	snprintf(stream_name, sizeof(stream_name), "%s", info->name);

	abox_dump_card.dev = dev;
	link->name = devm_kmemdup(dev, stream_name, sizeof(stream_name), GFP_KERNEL);
	link->stream_name = devm_kmemdup(dev, stream_name, sizeof(stream_name), GFP_KERNEL);
	link->cpu_name = "snd-soc-dummy";
	link->cpu_dai_name = "snd-soc-dummy-dai";
	link->platform_name = dev_name(dev);
	link->codec_name = "snd-soc-dummy";
	link->codec_dai_name = "snd-soc-dummy-dai";
	link->ignore_suspend = 1;
	link->ignore_pmdown_time = 1;
	link->capture_only = true;
	schedule_delayed_work(&abox_dump_register_card_work, msecs_to_jiffies(10 * MSEC_PER_SEC));
}

static int abox_dump_platform_probe(struct snd_soc_platform *platform)
{
	struct device *dev = platform->dev;
	int id = to_platform_device(dev)->id;

	dev_dbg(dev, "%s[%d]\n", __func__, id);

	return 0;
}

static int abox_dump_platform_new(struct snd_soc_pcm_runtime *runtime)
{
	struct device *dev = runtime->platform->dev;
	struct snd_pcm_substream *substream = runtime->pcm->streams[SNDRV_PCM_STREAM_CAPTURE].substream;
	struct snd_dma_buffer *dmab = &substream->dma_buffer;
	int id = to_platform_device(dev)->id;
	struct abox_dump_buffer_info *info = abox_dump_get_buffer_info(id);

	dev_dbg(dev, "%s[%d]\n", __func__, id);

	dmab->dev.type = SNDRV_DMA_TYPE_DEV;
	dmab->dev.dev = dev;
	dmab->area = info->buffer.area;
	dmab->addr = info->buffer.addr;
	dmab->bytes = info->buffer.bytes;

	return 0;
}

static void abox_dump_platform_free(struct snd_pcm *pcm)
{
	struct snd_soc_pcm_runtime *runtime =
			pcm->streams[SNDRV_PCM_STREAM_PLAYBACK].substream->private_data;
	struct device *dev = runtime->platform->dev;
	int id = to_platform_device(dev)->id;

	dev_dbg(dev, "%s[%d]\n", __func__, id);
}

struct snd_soc_platform_driver abox_dump_platform = {
	.probe		= abox_dump_platform_probe,
	.ops		= &abox_dump_platform_ops,
	.pcm_new	= abox_dump_platform_new,
	.pcm_free	= abox_dump_platform_free,
};

static int samsung_abox_dump_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int id = to_platform_device(dev)->id;

	dev_dbg(dev, "%s[%d]\n", __func__, id);

	devm_snd_soc_register_platform(dev, &abox_dump_platform);
	abox_dump_add_dai_link(dev);

	return 0;
}

static int samsung_abox_dump_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int id = to_platform_device(dev)->id;

	dev_dbg(dev, "%s[%d]\n", __func__, id);

	return 0;
}

static const struct platform_device_id samsung_dump_driver_ids[] = {
	{
		.name = "samsung-abox-dump",
	},
	{},
};
MODULE_DEVICE_TABLE(platform, samsung_dump_driver_ids);

static struct platform_driver samsung_abox_dump_driver = {
	.probe  = samsung_abox_dump_probe,
	.remove = samsung_abox_dump_remove,
	.driver = {
		.name = "samsung-abox-dump",
		.owner = THIS_MODULE,
	},
	.id_table = samsung_dump_driver_ids,
};

module_platform_driver(samsung_abox_dump_driver);

/* Module information */
MODULE_AUTHOR("Gyeongtaek Lee, <gt82.lee@samsung.com>");
MODULE_DESCRIPTION("Samsung ASoC A-Box Internal Buffer Dumping Driver");
MODULE_ALIAS("platform:samsung-abox-dump");
MODULE_LICENSE("GPL");

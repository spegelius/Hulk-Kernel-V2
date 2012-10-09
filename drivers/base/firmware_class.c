/*
 * firmware_class.c - Multi purpose firmware loading support
 *
 * Copyright (c) 2003 Manuel Estrada Sainz
 *
 * Please see Documentation/firmware_class/ for more information.
 *
 */

#include <linux/capability.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/timer.h>
#include <linux/vmalloc.h>
#include <linux/interrupt.h>
#include <linux/bitops.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/highmem.h>
#include <linux/firmware.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/file.h>
#include <linux/list.h>
#include <linux/async.h>
#include <linux/pm.h>
#include <linux/suspend.h>
#include <linux/syscore_ops.h>

#include <generated/utsrelease.h>

#include "base.h"

MODULE_AUTHOR("Manuel Estrada Sainz");
MODULE_DESCRIPTION("Multi purpose firmware loading support");
MODULE_LICENSE("GPL");

static const char *fw_path[] = {
	"/lib/firmware/updates/" UTS_RELEASE,
	"/lib/firmware/updates",
	"/lib/firmware/" UTS_RELEASE,
	"/lib/firmware"
};

/* Don't inline this: 'struct kstat' is biggish */
static noinline long fw_file_size(struct file *file)
{
	struct kstat st;
	if (vfs_getattr(file->f_path.mnt, file->f_path.dentry, &st))
		return -1;
	if (!S_ISREG(st.mode))
		return -1;
	if (st.size != (long)st.size)
		return -1;
	return st.size;
}

static bool fw_read_file_contents(struct file *file, struct firmware *fw)
{
	long size;
	char *buf;

	size = fw_file_size(file);
	if (size < 0)
		return false;
	buf = vmalloc(size);
	if (!buf)
		return false;
	if (kernel_read(file, 0, buf, size) != size) {
		vfree(buf);
		return false;
	}
	fw->data = buf;
	fw->size = size;
	return true;
}

static bool fw_get_filesystem_firmware(struct firmware *fw, const char *name)
{
	int i;
	bool success = false;
	char *path = __getname();

	for (i = 0; i < ARRAY_SIZE(fw_path); i++) {
		struct file *file;
		snprintf(path, PATH_MAX, "%s/%s", fw_path[i], name);

		file = filp_open(path, O_RDONLY, 0);
		if (IS_ERR(file))
			continue;
		success = fw_read_file_contents(file, fw);
		fput(file);
		if (success)
			break;
	}
	__putname(path);
	return success;
}

/* Builtin firmware support */

#ifdef CONFIG_FW_LOADER

extern struct builtin_fw __start_builtin_fw[];
extern struct builtin_fw __end_builtin_fw[];

static bool fw_get_builtin_firmware(struct firmware *fw, const char *name)
{
	struct builtin_fw *b_fw;

	for (b_fw = __start_builtin_fw; b_fw != __end_builtin_fw; b_fw++) {
		if (strcmp(name, b_fw->name) == 0) {
			fw->size = b_fw->size;
			fw->data = b_fw->data;
			return true;
		}
	}

	return false;
}

static bool fw_is_builtin_firmware(const struct firmware *fw)
{
	struct builtin_fw *b_fw;

	for (b_fw = __start_builtin_fw; b_fw != __end_builtin_fw; b_fw++)
		if (fw->data == b_fw->data)
			return true;

	return false;
}

#else /* Module case - no builtin firmware support */

static inline bool fw_get_builtin_firmware(struct firmware *fw, const char *name)
{
	return false;
}

static inline bool fw_is_builtin_firmware(const struct firmware *fw)
{
	return false;
}
#endif

enum {
	FW_STATUS_LOADING,
	FW_STATUS_DONE,
	FW_STATUS_ABORT,
};

static int loading_timeout = 60;	/* In seconds */

static inline long firmware_loading_timeout(void)
{
	return loading_timeout > 0 ? loading_timeout * HZ : MAX_SCHEDULE_TIMEOUT;
}

struct firmware_cache {
	/* firmware_buf instance will be added into the below list */
	spinlock_t lock;
	struct list_head head;
	int state;

#ifdef CONFIG_PM_SLEEP
	/*
	 * Names of firmware images which have been cached successfully
	 * will be added into the below list so that device uncache
	 * helper can trace which firmware images have been cached
	 * before.
	 */
	spinlock_t name_lock;
	struct list_head fw_names;

	wait_queue_head_t wait_queue;
	int cnt;
	struct delayed_work work;

	struct notifier_block   pm_notify;
#endif
};

struct firmware_buf {
	struct kref ref;
	struct list_head list;
	struct completion completion;
	struct firmware_cache *fwc;
	unsigned long status;
	void *data;
	size_t size;
	struct page **pages;
	int nr_pages;
	int page_array_size;
	char fw_id[];
};

struct fw_cache_entry {
	struct list_head list;
	char name[];
};

struct firmware_priv {
	struct timer_list timeout;
	bool nowait;
	struct device dev;
	struct firmware_buf *buf;
	struct firmware *fw;
};

struct fw_name_devm {
	unsigned long magic;
	char name[];
};

#define to_fwbuf(d) container_of(d, struct firmware_buf, ref)

#define	FW_LOADER_NO_CACHE	0
#define	FW_LOADER_START_CACHE	1

static int fw_cache_piggyback_on_request(const char *name);

/* fw_lock could be moved to 'struct firmware_priv' but since it is just
 * guarding for corner cases a global lock should be OK */
static DEFINE_MUTEX(fw_lock);

static struct firmware_cache fw_cache;

static struct firmware_buf *__allocate_fw_buf(const char *fw_name,
					      struct firmware_cache *fwc)
{
	struct firmware_buf *buf;

	buf = kzalloc(sizeof(*buf) + strlen(fw_name) + 1 , GFP_ATOMIC);

	if (!buf)
		return buf;

	kref_init(&buf->ref);
	strcpy(buf->fw_id, fw_name);
	buf->fwc = fwc;
	init_completion(&buf->completion);

	pr_debug("%s: fw-%s buf=%p\n", __func__, fw_name, buf);

	return buf;
}

static struct firmware_buf *__fw_lookup_buf(const char *fw_name)
{
	struct firmware_buf *tmp;
	struct firmware_cache *fwc = &fw_cache;

	list_for_each_entry(tmp, &fwc->head, list)
		if (!strcmp(tmp->fw_id, fw_name))
			return tmp;
	return NULL;
}

static int fw_lookup_and_allocate_buf(const char *fw_name,
				      struct firmware_cache *fwc,
				      struct firmware_buf **buf)
{
	struct firmware_buf *tmp;

	spin_lock(&fwc->lock);
	tmp = __fw_lookup_buf(fw_name);
	if (tmp) {
		kref_get(&tmp->ref);
		spin_unlock(&fwc->lock);
		*buf = tmp;
		return 1;
	}
	tmp = __allocate_fw_buf(fw_name, fwc);
	if (tmp)
		list_add(&tmp->list, &fwc->head);
	spin_unlock(&fwc->lock);

	*buf = tmp;

	return tmp ? 0 : -ENOMEM;
}

static struct firmware_buf *fw_lookup_buf(const char *fw_name)
{
	struct firmware_buf *tmp;
	struct firmware_cache *fwc = &fw_cache;

	spin_lock(&fwc->lock);
	tmp = __fw_lookup_buf(fw_name);
	spin_unlock(&fwc->lock);

	return tmp;
}

static void __fw_free_buf(struct kref *ref)
{
	struct firmware_buf *buf = to_fwbuf(ref);
	struct firmware_cache *fwc = buf->fwc;
	int i;

	pr_debug("%s: fw-%s buf=%p data=%p size=%u\n",
		 __func__, buf->fw_id, buf, buf->data,
		 (unsigned int)buf->size);

	spin_lock(&fwc->lock);
	list_del(&buf->list);
	spin_unlock(&fwc->lock);

	vunmap(buf->data);
	for (i = 0; i < buf->nr_pages; i++)
		__free_page(buf->pages[i]);
	kfree(buf->pages);
	kfree(buf);
}

static void fw_free_buf(struct firmware_buf *buf)
{
	kref_put(&buf->ref, __fw_free_buf);
}

static struct firmware_priv *to_firmware_priv(struct device *dev)
{
	return container_of(dev, struct firmware_priv, dev);
}

static void fw_load_abort(struct firmware_priv *fw_priv)
{
	struct firmware_buf *buf = fw_priv->buf;

	set_bit(FW_STATUS_ABORT, &buf->status);
	complete_all(&buf->completion);
}

static ssize_t firmware_timeout_show(struct class *class,
				     struct class_attribute *attr,
				     char *buf)
{
	return sprintf(buf, "%d\n", loading_timeout);
}

/**
 * firmware_timeout_store - set number of seconds to wait for firmware
 * @class: device class pointer
 * @attr: device attribute pointer
 * @buf: buffer to scan for timeout value
 * @count: number of bytes in @buf
 *
 *	Sets the number of seconds to wait for the firmware.  Once
 *	this expires an error will be returned to the driver and no
 *	firmware will be provided.
 *
 *	Note: zero means 'wait forever'.
 **/
static ssize_t firmware_timeout_store(struct class *class,
				      struct class_attribute *attr,
				      const char *buf, size_t count)
{
	loading_timeout = simple_strtol(buf, NULL, 10);
	if (loading_timeout < 0)
		loading_timeout = 0;

	return count;
}

static struct class_attribute firmware_class_attrs[] = {
	__ATTR(timeout, S_IWUSR | S_IRUGO,
		firmware_timeout_show, firmware_timeout_store),
	__ATTR_NULL
};

static void fw_dev_release(struct device *dev)
{
	struct firmware_priv *fw_priv = to_firmware_priv(dev);

	kfree(fw_priv);

	module_put(THIS_MODULE);
}

static int firmware_uevent(struct device *dev, struct kobj_uevent_env *env)
{
	struct firmware_priv *fw_priv = to_firmware_priv(dev);

	if (add_uevent_var(env, "FIRMWARE=%s", fw_priv->buf->fw_id))
		return -ENOMEM;
	if (add_uevent_var(env, "TIMEOUT=%i", loading_timeout))
		return -ENOMEM;
	if (add_uevent_var(env, "ASYNC=%d", fw_priv->nowait))
		return -ENOMEM;

	return 0;
}

static struct class firmware_class = {
	.name		= "firmware",
	.class_attrs	= firmware_class_attrs,
	.dev_uevent	= firmware_uevent,
	.dev_release	= fw_dev_release,
};

static ssize_t firmware_loading_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct firmware_priv *fw_priv = to_firmware_priv(dev);
	int loading = test_bit(FW_STATUS_LOADING, &fw_priv->buf->status);

	return sprintf(buf, "%d\n", loading);
}

/* firmware holds the ownership of pages */
static void firmware_free_data(const struct firmware *fw)
{
	/* Loaded directly? */
	if (!fw->priv) {
		vfree(fw->data);
		return;
	}
	fw_free_buf(fw->priv);
}

/* Some architectures don't have PAGE_KERNEL_RO */
#ifndef PAGE_KERNEL_RO
#define PAGE_KERNEL_RO PAGE_KERNEL
#endif
/**
 * firmware_loading_store - set value in the 'loading' control file
 * @dev: device pointer
 * @attr: device attribute pointer
 * @buf: buffer to scan for loading control value
 * @count: number of bytes in @buf
 *
 *	The relevant values are:
 *
 *	 1: Start a load, discarding any previous partial load.
 *	 0: Conclude the load and hand the data to the driver code.
 *	-1: Conclude the load with an error and discard any written data.
 **/
static ssize_t firmware_loading_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct firmware_priv *fw_priv = to_firmware_priv(dev);
	struct firmware_buf *fw_buf = fw_priv->buf;
	int loading = simple_strtol(buf, NULL, 10);
	int i;

	mutex_lock(&fw_lock);

	if (!fw_buf)
		goto out;

	switch (loading) {
	case 1:
		/* discarding any previous partial load */
		if (!test_bit(FW_STATUS_DONE, &fw_buf->status)) {
			for (i = 0; i < fw_buf->nr_pages; i++)
				__free_page(fw_buf->pages[i]);
			kfree(fw_buf->pages);
			fw_buf->pages = NULL;
			fw_buf->page_array_size = 0;
			fw_buf->nr_pages = 0;
			set_bit(FW_STATUS_LOADING, &fw_buf->status);
		}
		break;
	case 0:
		if (test_bit(FW_STATUS_LOADING, &fw_buf->status)) {
			set_bit(FW_STATUS_DONE, &fw_buf->status);
			clear_bit(FW_STATUS_LOADING, &fw_buf->status);
			complete_all(&fw_buf->completion);
			break;
		}
		/* fallthrough */
	default:
		dev_err(dev, "%s: unexpected value (%d)\n", __func__, loading);
		/* fallthrough */
	case -1:
		fw_load_abort(fw_priv);
		break;
	}
out:
	mutex_unlock(&fw_lock);
	return count;
}

static DEVICE_ATTR(loading, 0644, firmware_loading_show, firmware_loading_store);

static ssize_t firmware_data_read(struct file *filp, struct kobject *kobj,
				  struct bin_attribute *bin_attr,
				  char *buffer, loff_t offset, size_t count)
{
	struct device *dev = kobj_to_dev(kobj);
	struct firmware_priv *fw_priv = to_firmware_priv(dev);
	struct firmware_buf *buf;
	ssize_t ret_count;

	mutex_lock(&fw_lock);
	buf = fw_priv->buf;
	if (!buf || test_bit(FW_STATUS_DONE, &buf->status)) {
		ret_count = -ENODEV;
		goto out;
	}
	if (offset > buf->size) {
		ret_count = 0;
		goto out;
	}
	if (count > buf->size - offset)
		count = buf->size - offset;

	ret_count = count;

	while (count) {
		void *page_data;
		int page_nr = offset >> PAGE_SHIFT;
		int page_ofs = offset & (PAGE_SIZE-1);
		int page_cnt = min_t(size_t, PAGE_SIZE - page_ofs, count);

		page_data = kmap(buf->pages[page_nr]);

		memcpy(buffer, page_data + page_ofs, page_cnt);

		kunmap(buf->pages[page_nr]);
		buffer += page_cnt;
		offset += page_cnt;
		count -= page_cnt;
	}
out:
	mutex_unlock(&fw_lock);
	return ret_count;
}

static int fw_realloc_buffer(struct firmware_priv *fw_priv, int min_size)
{
	struct firmware_buf *buf = fw_priv->buf;
	int pages_needed = ALIGN(min_size, PAGE_SIZE) >> PAGE_SHIFT;

	/* If the array of pages is too small, grow it... */
	if (buf->page_array_size < pages_needed) {
		int new_array_size = max(pages_needed,
					 buf->page_array_size * 2);
		struct page **new_pages;

		new_pages = kmalloc(new_array_size * sizeof(void *),
				    GFP_KERNEL);
		if (!new_pages) {
			fw_load_abort(fw_priv);
			return -ENOMEM;
		}
		memcpy(new_pages, buf->pages,
		       buf->page_array_size * sizeof(void *));
		memset(&new_pages[buf->page_array_size], 0, sizeof(void *) *
		       (new_array_size - buf->page_array_size));
		kfree(buf->pages);
		buf->pages = new_pages;
		buf->page_array_size = new_array_size;
	}

	while (buf->nr_pages < pages_needed) {
		buf->pages[buf->nr_pages] =
			alloc_page(GFP_KERNEL | __GFP_HIGHMEM);

		if (!buf->pages[buf->nr_pages]) {
			fw_load_abort(fw_priv);
			return -ENOMEM;
		}
		buf->nr_pages++;
	}
	return 0;
}

/**
 * firmware_data_write - write method for firmware
 * @filp: open sysfs file
 * @kobj: kobject for the device
 * @bin_attr: bin_attr structure
 * @buffer: buffer being written
 * @offset: buffer offset for write in total data store area
 * @count: buffer size
 *
 *	Data written to the 'data' attribute will be later handed to
 *	the driver as a firmware image.
 **/
static ssize_t firmware_data_write(struct file *filp, struct kobject *kobj,
				   struct bin_attribute *bin_attr,
				   char *buffer, loff_t offset, size_t count)
{
	struct device *dev = kobj_to_dev(kobj);
	struct firmware_priv *fw_priv = to_firmware_priv(dev);
	struct firmware_buf *buf;
	ssize_t retval;

	if (!capable(CAP_SYS_RAWIO))
		return -EPERM;

	mutex_lock(&fw_lock);
	buf = fw_priv->buf;
	if (!buf || test_bit(FW_STATUS_DONE, &buf->status)) {
		retval = -ENODEV;
		goto out;
	}

	retval = fw_realloc_buffer(fw_priv, offset + count);
	if (retval)
		goto out;

	retval = count;

	while (count) {
		void *page_data;
		int page_nr = offset >> PAGE_SHIFT;
		int page_ofs = offset & (PAGE_SIZE - 1);
		int page_cnt = min_t(size_t, PAGE_SIZE - page_ofs, count);

		page_data = kmap(buf->pages[page_nr]);

		memcpy(page_data + page_ofs, buffer, page_cnt);

		kunmap(buf->pages[page_nr]);
		buffer += page_cnt;
		offset += page_cnt;
		count -= page_cnt;
	}

	buf->size = max_t(size_t, offset, buf->size);
out:
	mutex_unlock(&fw_lock);
	return retval;
}

static struct bin_attribute firmware_attr_data = {
	.attr = { .name = "data", .mode = 0644 },
	.size = 0,
	.read = firmware_data_read,
	.write = firmware_data_write,
};

static void firmware_class_timeout(u_long data)
{
	struct firmware_priv *fw_priv = (struct firmware_priv *) data;

	fw_load_abort(fw_priv);
}

static struct firmware_priv *
fw_create_instance(struct firmware *firmware, const char *fw_name,
		   struct device *device, bool uevent, bool nowait)
{
	struct firmware_priv *fw_priv;
	struct device *f_dev;

	fw_priv = kzalloc(sizeof(*fw_priv), GFP_KERNEL);
	if (!fw_priv) {
		dev_err(device, "%s: kmalloc failed\n", __func__);
		fw_priv = ERR_PTR(-ENOMEM);
		goto exit;
	}

	fw_priv->nowait = nowait;
	fw_priv->fw = firmware;
	setup_timer(&fw_priv->timeout,
		    firmware_class_timeout, (u_long) fw_priv);

	f_dev = &fw_priv->dev;

	device_initialize(f_dev);
	dev_set_name(f_dev, "%s", fw_name);
	f_dev->parent = device;
	f_dev->class = &firmware_class;
exit:
	return fw_priv;
}

/* one pages buffer is mapped/unmapped only once */
static int fw_map_pages_buf(struct firmware_buf *buf)
{
	buf->data = vmap(buf->pages, buf->nr_pages, 0, PAGE_KERNEL_RO);
	if (!buf->data)
		return -ENOMEM;
	return 0;
}

/* store the pages buffer info firmware from buf */
static void fw_set_page_data(struct firmware_buf *buf, struct firmware *fw)
{
	fw->priv = buf;
	fw->pages = buf->pages;
	fw->size = buf->size;
	fw->data = buf->data;

	pr_debug("%s: fw-%s buf=%p data=%p size=%u\n",
		 __func__, buf->fw_id, buf, buf->data,
		 (unsigned int)buf->size);
}

#ifdef CONFIG_PM_SLEEP
static void fw_name_devm_release(struct device *dev, void *res)
{
	struct fw_name_devm *fwn = res;

	if (fwn->magic == (unsigned long)&fw_cache)
		pr_debug("%s: fw_name-%s devm-%p released\n",
				__func__, fwn->name, res);
}

static int fw_devm_match(struct device *dev, void *res,
		void *match_data)
{
	struct fw_name_devm *fwn = res;

	return (fwn->magic == (unsigned long)&fw_cache) &&
		!strcmp(fwn->name, match_data);
}

static struct fw_name_devm *fw_find_devm_name(struct device *dev,
		const char *name)
{
	struct fw_name_devm *fwn;

	fwn = devres_find(dev, fw_name_devm_release,
			  fw_devm_match, (void *)name);
	return fwn;
}

/* add firmware name into devres list */
static int fw_add_devm_name(struct device *dev, const char *name)
{
	struct fw_name_devm *fwn;

	fwn = fw_find_devm_name(dev, name);
	if (fwn)
		return 1;

	fwn = devres_alloc(fw_name_devm_release, sizeof(struct fw_name_devm) +
			   strlen(name) + 1, GFP_KERNEL);
	if (!fwn)
		return -ENOMEM;

	fwn->magic = (unsigned long)&fw_cache;
	strcpy(fwn->name, name);
	devres_add(dev, fwn);

	return 0;
}
#else
static int fw_add_devm_name(struct device *dev, const char *name)
{
	return 0;
}
#endif

static void _request_firmware_cleanup(const struct firmware **firmware_p)
{
	release_firmware(*firmware_p);
	*firmware_p = NULL;
}

static struct firmware_priv *
_request_firmware_prepare(const struct firmware **firmware_p, const char *name,
			  struct device *device, bool uevent, bool nowait)
{
	struct firmware *firmware;
	struct firmware_priv *fw_priv = NULL;
	struct firmware_buf *buf;
	int ret;

	if (!firmware_p)
		return ERR_PTR(-EINVAL);

	*firmware_p = firmware = kzalloc(sizeof(*firmware), GFP_KERNEL);
	if (!firmware) {
		dev_err(device, "%s: kmalloc(struct firmware) failed\n",
			__func__);
		return ERR_PTR(-ENOMEM);
	}

	if (fw_get_builtin_firmware(firmware, name)) {
		dev_dbg(device, "firmware: using built-in firmware %s\n", name);
		return NULL;
	}

	if (fw_get_filesystem_firmware(firmware, name)) {
		dev_dbg(device, "firmware: direct-loading firmware %s\n", name);
		return NULL;
	}

	ret = fw_lookup_and_allocate_buf(name, &fw_cache, &buf);
	if (!ret)
		fw_priv = fw_create_instance(firmware, name, device,
				uevent, nowait);

	if (IS_ERR(fw_priv) || ret < 0) {
		kfree(firmware);
		*firmware_p = NULL;
		return ERR_PTR(-ENOMEM);
	} else if (fw_priv) {
		fw_priv->buf = buf;

		/*
		 * bind with 'buf' now to avoid warning in failure path
		 * of requesting firmware.
		 */
		firmware->priv = buf;
		return fw_priv;
	}

	/* share the cached buf, which is inprogessing or completed */
 check_status:
	mutex_lock(&fw_lock);
	if (test_bit(FW_STATUS_ABORT, &buf->status)) {
		fw_priv = ERR_PTR(-ENOENT);
		firmware->priv = buf;
		_request_firmware_cleanup(firmware_p);
		goto exit;
	} else if (test_bit(FW_STATUS_DONE, &buf->status)) {
		fw_priv = NULL;
		fw_set_page_data(buf, firmware);
		goto exit;
	}
	mutex_unlock(&fw_lock);
	wait_for_completion(&buf->completion);
	goto check_status;

exit:
	mutex_unlock(&fw_lock);
	return fw_priv;
}

static int _request_firmware_load(struct firmware_priv *fw_priv, bool uevent,
				  long timeout)
{
	int retval = 0;
	struct device *f_dev = &fw_priv->dev;
	struct firmware_buf *buf = fw_priv->buf;
	struct firmware_cache *fwc = &fw_cache;

	dev_set_uevent_suppress(f_dev, true);

	/* Need to pin this module until class device is destroyed */
	__module_get(THIS_MODULE);

	retval = device_add(f_dev);
	if (retval) {
		dev_err(f_dev, "%s: device_register failed\n", __func__);
		goto err_put_dev;
	}

	retval = device_create_bin_file(f_dev, &firmware_attr_data);
	if (retval) {
		dev_err(f_dev, "%s: sysfs_create_bin_file failed\n", __func__);
		goto err_del_dev;
	}

	retval = device_create_file(f_dev, &dev_attr_loading);
	if (retval) {
		dev_err(f_dev, "%s: device_create_file failed\n", __func__);
		goto err_del_bin_attr;
	}

	if (uevent) {
		dev_set_uevent_suppress(f_dev, false);
		dev_dbg(f_dev, "firmware: requesting %s\n", buf->fw_id);
		if (timeout != MAX_SCHEDULE_TIMEOUT)
			mod_timer(&fw_priv->timeout,
				  round_jiffies_up(jiffies + timeout));

		kobject_uevent(&fw_priv->dev.kobj, KOBJ_ADD);
	}

	wait_for_completion(&buf->completion);

	del_timer_sync(&fw_priv->timeout);

	mutex_lock(&fw_lock);
	if (!buf->size || test_bit(FW_STATUS_ABORT, &buf->status))
		retval = -ENOENT;

	/*
	 * add firmware name into devres list so that we can auto cache
	 * and uncache firmware for device.
	 *
	 * f_dev->parent may has been deleted already, but the problem
	 * should be fixed in devres or driver core.
	 */
	if (!retval && f_dev->parent)
		fw_add_devm_name(f_dev->parent, buf->fw_id);

	if (!retval)
		retval = fw_map_pages_buf(buf);

	/*
	 * After caching firmware image is started, let it piggyback
	 * on request firmware.
	 */
	if (!retval && fwc->state == FW_LOADER_START_CACHE) {
		if (fw_cache_piggyback_on_request(buf->fw_id))
			kref_get(&buf->ref);
	}

	/* pass the pages buffer to driver at the last minute */
	fw_set_page_data(buf, fw_priv->fw);

	fw_priv->buf = NULL;
	mutex_unlock(&fw_lock);

	device_remove_file(f_dev, &dev_attr_loading);
err_del_bin_attr:
	device_remove_bin_file(f_dev, &firmware_attr_data);
err_del_dev:
	device_del(f_dev);
err_put_dev:
	put_device(f_dev);
	return retval;
}

/**
 * request_firmware: - send firmware request and wait for it
 * @firmware_p: pointer to firmware image
 * @name: name of firmware file
 * @device: device for which firmware is being loaded
 *
 *      @firmware_p will be used to return a firmware image by the name
 *      of @name for device @device.
 *
 *      Should be called from user context where sleeping is allowed.
 *
 *      @name will be used as $FIRMWARE in the uevent environment and
 *      should be distinctive enough not to be confused with any other
 *      firmware image for this or any other device.
 *
 *	Caller must hold the reference count of @device.
 **/
int
request_firmware(const struct firmware **firmware_p, const char *name,
                 struct device *device)
{
	struct firmware_priv *fw_priv;
	int ret;

	if (!name || name[0] == '\0')
		return -EINVAL;

	fw_priv = _request_firmware_prepare(firmware_p, name, device, true,
					    false);
	if (IS_ERR_OR_NULL(fw_priv))
		return PTR_RET(fw_priv);

	ret = usermodehelper_read_trylock();
	if (WARN_ON(ret)) {
		dev_err(device, "firmware: %s will not be loaded\n", name);
	} else {
		ret = _request_firmware_load(fw_priv, true,
					firmware_loading_timeout());
		usermodehelper_read_unlock();
	}
	if (ret)
		_request_firmware_cleanup(firmware_p);

	return ret;
}

/**
 * release_firmware: - release the resource associated with a firmware image
 * @fw: firmware resource to release
 **/
void release_firmware(const struct firmware *fw)
{
	if (fw) {
		if (!fw_is_builtin_firmware(fw))
			firmware_free_data(fw);
		kfree(fw);
	}
}

/* Async support */
struct firmware_work {
	struct work_struct work;
	struct module *module;
	const char *name;
	struct device *device;
	void *context;
	void (*cont)(const struct firmware *fw, void *context);
	bool uevent;
};

static void request_firmware_work_func(struct work_struct *work)
{
	struct firmware_work *fw_work;
	const struct firmware *fw;
	struct firmware_priv *fw_priv;
	long timeout;
	int ret;

	fw_work = container_of(work, struct firmware_work, work);
	fw_priv = _request_firmware_prepare(&fw, fw_work->name, fw_work->device,
			fw_work->uevent, true);
	if (IS_ERR_OR_NULL(fw_priv)) {
		ret = PTR_RET(fw_priv);
		goto out;
	}

	timeout = usermodehelper_read_lock_wait(firmware_loading_timeout());
	if (timeout) {
		ret = _request_firmware_load(fw_priv, fw_work->uevent, timeout);
		usermodehelper_read_unlock();
	} else {
		dev_dbg(fw_work->device, "firmware: %s loading timed out\n",
			fw_work->name);
		ret = -EAGAIN;
	}
	if (ret)
		_request_firmware_cleanup(&fw);

 out:
	fw_work->cont(fw, fw_work->context);
	put_device(fw_work->device);

	module_put(fw_work->module);
	kfree(fw_work);
}

/**
 * request_firmware_nowait - asynchronous version of request_firmware
 * @module: module requesting the firmware
 * @uevent: sends uevent to copy the firmware image if this flag
 *	is non-zero else the firmware copy must be done manually.
 * @name: name of firmware file
 * @device: device for which firmware is being loaded
 * @gfp: allocation flags
 * @context: will be passed over to @cont, and
 *	@fw may be %NULL if firmware request fails.
 * @cont: function will be called asynchronously when the firmware
 *	request is over.
 *
 *	Caller must hold the reference count of @device.
 *
 *	Asynchronous variant of request_firmware() for user contexts:
 *		- sleep for as small periods as possible since it may
 *		increase kernel boot time of built-in device drivers
 *		requesting firmware in their ->probe() methods, if
 *		@gfp is GFP_KERNEL.
 *
 *		- can't sleep at all if @gfp is GFP_ATOMIC.
 **/
int
request_firmware_nowait(
	struct module *module, bool uevent,
	const char *name, struct device *device, gfp_t gfp, void *context,
	void (*cont)(const struct firmware *fw, void *context))
{
	struct firmware_work *fw_work;

	fw_work = kzalloc(sizeof (struct firmware_work), gfp);
	if (!fw_work)
		return -ENOMEM;

	fw_work->module = module;
	fw_work->name = name;
	fw_work->device = device;
	fw_work->context = context;
	fw_work->cont = cont;
	fw_work->uevent = uevent;

	if (!try_module_get(module)) {
		kfree(fw_work);
		return -EFAULT;
	}

	get_device(fw_work->device);
	INIT_WORK(&fw_work->work, request_firmware_work_func);
	schedule_work(&fw_work->work);
	return 0;
}

/**
 * cache_firmware - cache one firmware image in kernel memory space
 * @fw_name: the firmware image name
 *
 * Cache firmware in kernel memory so that drivers can use it when
 * system isn't ready for them to request firmware image from userspace.
 * Once it returns successfully, driver can use request_firmware or its
 * nowait version to get the cached firmware without any interacting
 * with userspace
 *
 * Return 0 if the firmware image has been cached successfully
 * Return !0 otherwise
 *
 */
int cache_firmware(const char *fw_name)
{
	int ret;
	const struct firmware *fw;

	pr_debug("%s: %s\n", __func__, fw_name);

	ret = request_firmware(&fw, fw_name, NULL);
	if (!ret)
		kfree(fw);

	pr_debug("%s: %s ret=%d\n", __func__, fw_name, ret);

	return ret;
}

/**
 * uncache_firmware - remove one cached firmware image
 * @fw_name: the firmware image name
 *
 * Uncache one firmware image which has been cached successfully
 * before.
 *
 * Return 0 if the firmware cache has been removed successfully
 * Return !0 otherwise
 *
 */
int uncache_firmware(const char *fw_name)
{
	struct firmware_buf *buf;
	struct firmware fw;

	pr_debug("%s: %s\n", __func__, fw_name);

	if (fw_get_builtin_firmware(&fw, fw_name))
		return 0;

	buf = fw_lookup_buf(fw_name);
	if (buf) {
		fw_free_buf(buf);
		return 0;
	}

	return -EINVAL;
}

#ifdef CONFIG_PM_SLEEP
static struct fw_cache_entry *alloc_fw_cache_entry(const char *name)
{
	struct fw_cache_entry *fce;

	fce = kzalloc(sizeof(*fce) + strlen(name) + 1, GFP_ATOMIC);
	if (!fce)
		goto exit;

	strcpy(fce->name, name);
exit:
	return fce;
}

static int __fw_entry_found(const char *name)
{
	struct firmware_cache *fwc = &fw_cache;
	struct fw_cache_entry *fce;

	list_for_each_entry(fce, &fwc->fw_names, list) {
		if (!strcmp(fce->name, name))
			return 1;
	}
	return 0;
}

static int fw_cache_piggyback_on_request(const char *name)
{
	struct firmware_cache *fwc = &fw_cache;
	struct fw_cache_entry *fce;
	int ret = 0;

	spin_lock(&fwc->name_lock);
	if (__fw_entry_found(name))
		goto found;

	fce = alloc_fw_cache_entry(name);
	if (fce) {
		ret = 1;
		list_add(&fce->list, &fwc->fw_names);
		pr_debug("%s: fw: %s\n", __func__, name);
	}
found:
	spin_unlock(&fwc->name_lock);
	return ret;
}

static void free_fw_cache_entry(struct fw_cache_entry *fce)
{
	kfree(fce);
}

static void __async_dev_cache_fw_image(void *fw_entry,
				       async_cookie_t cookie)
{
	struct fw_cache_entry *fce = fw_entry;
	struct firmware_cache *fwc = &fw_cache;
	int ret;

	ret = cache_firmware(fce->name);
	if (ret) {
		spin_lock(&fwc->name_lock);
		list_del(&fce->list);
		spin_unlock(&fwc->name_lock);

		free_fw_cache_entry(fce);
	}

	spin_lock(&fwc->name_lock);
	fwc->cnt--;
	spin_unlock(&fwc->name_lock);

	wake_up(&fwc->wait_queue);
}

/* called with dev->devres_lock held */
static void dev_create_fw_entry(struct device *dev, void *res,
				void *data)
{
	struct fw_name_devm *fwn = res;
	const char *fw_name = fwn->name;
	struct list_head *head = data;
	struct fw_cache_entry *fce;

	fce = alloc_fw_cache_entry(fw_name);
	if (fce)
		list_add(&fce->list, head);
}

static int devm_name_match(struct device *dev, void *res,
			   void *match_data)
{
	struct fw_name_devm *fwn = res;
	return (fwn->magic == (unsigned long)match_data);
}

static void dev_cache_fw_image(struct device *dev, void *data)
{
	LIST_HEAD(todo);
	struct fw_cache_entry *fce;
	struct fw_cache_entry *fce_next;
	struct firmware_cache *fwc = &fw_cache;

	devres_for_each_res(dev, fw_name_devm_release,
			    devm_name_match, &fw_cache,
			    dev_create_fw_entry, &todo);

	list_for_each_entry_safe(fce, fce_next, &todo, list) {
		list_del(&fce->list);

		spin_lock(&fwc->name_lock);
		/* only one cache entry for one firmware */
		if (!__fw_entry_found(fce->name)) {
			fwc->cnt++;
			list_add(&fce->list, &fwc->fw_names);
		} else {
			free_fw_cache_entry(fce);
			fce = NULL;
		}
		spin_unlock(&fwc->name_lock);

		if (fce)
			async_schedule(__async_dev_cache_fw_image,
				       (void *)fce);
	}
}

static void __device_uncache_fw_images(void)
{
	struct firmware_cache *fwc = &fw_cache;
	struct fw_cache_entry *fce;

	spin_lock(&fwc->name_lock);
	while (!list_empty(&fwc->fw_names)) {
		fce = list_entry(fwc->fw_names.next,
				struct fw_cache_entry, list);
		list_del(&fce->list);
		spin_unlock(&fwc->name_lock);

		uncache_firmware(fce->name);
		free_fw_cache_entry(fce);

		spin_lock(&fwc->name_lock);
	}
	spin_unlock(&fwc->name_lock);
}

/**
 * device_cache_fw_images - cache devices' firmware
 *
 * If one device called request_firmware or its nowait version
 * successfully before, the firmware names are recored into the
 * device's devres link list, so device_cache_fw_images can call
 * cache_firmware() to cache these firmwares for the device,
 * then the device driver can load its firmwares easily at
 * time when system is not ready to complete loading firmware.
 */
static void device_cache_fw_images(void)
{
	struct firmware_cache *fwc = &fw_cache;
	int old_timeout;
	DEFINE_WAIT(wait);

	pr_debug("%s\n", __func__);

	/* cancel uncache work */
	cancel_delayed_work_sync(&fwc->work);

	/*
	 * use small loading timeout for caching devices' firmware
	 * because all these firmware images have been loaded
	 * successfully at lease once, also system is ready for
	 * completing firmware loading now. The maximum size of
	 * firmware in current distributions is about 2M bytes,
	 * so 10 secs should be enough.
	 */
	old_timeout = loading_timeout;
	loading_timeout = 10;

	mutex_lock(&fw_lock);
	fwc->state = FW_LOADER_START_CACHE;
	dpm_for_each_dev(NULL, dev_cache_fw_image);
	mutex_unlock(&fw_lock);

	/* wait for completion of caching firmware for all devices */
	spin_lock(&fwc->name_lock);
	for (;;) {
		prepare_to_wait(&fwc->wait_queue, &wait,
				TASK_UNINTERRUPTIBLE);
		if (!fwc->cnt)
			break;

		spin_unlock(&fwc->name_lock);

		schedule();

		spin_lock(&fwc->name_lock);
	}
	spin_unlock(&fwc->name_lock);
	finish_wait(&fwc->wait_queue, &wait);

	loading_timeout = old_timeout;
}

/**
 * device_uncache_fw_images - uncache devices' firmware
 *
 * uncache all firmwares which have been cached successfully
 * by device_uncache_fw_images earlier
 */
static void device_uncache_fw_images(void)
{
	pr_debug("%s\n", __func__);
	__device_uncache_fw_images();
}

static void device_uncache_fw_images_work(struct work_struct *work)
{
	device_uncache_fw_images();
}

/**
 * device_uncache_fw_images_delay - uncache devices firmwares
 * @delay: number of milliseconds to delay uncache device firmwares
 *
 * uncache all devices's firmwares which has been cached successfully
 * by device_cache_fw_images after @delay milliseconds.
 */
static void device_uncache_fw_images_delay(unsigned long delay)
{
	schedule_delayed_work(&fw_cache.work,
			msecs_to_jiffies(delay));
}

static int fw_pm_notify(struct notifier_block *notify_block,
			unsigned long mode, void *unused)
{
	switch (mode) {
	case PM_HIBERNATION_PREPARE:
	case PM_SUSPEND_PREPARE:
		device_cache_fw_images();
		break;

	case PM_POST_SUSPEND:
	case PM_POST_HIBERNATION:
	case PM_POST_RESTORE:
		/*
		 * In case that system sleep failed and syscore_suspend is
		 * not called.
		 */
		mutex_lock(&fw_lock);
		fw_cache.state = FW_LOADER_NO_CACHE;
		mutex_unlock(&fw_lock);

		device_uncache_fw_images_delay(10 * MSEC_PER_SEC);
		break;
	}

	return 0;
}

/* stop caching firmware once syscore_suspend is reached */
static int fw_suspend(void)
{
	fw_cache.state = FW_LOADER_NO_CACHE;
	return 0;
}

static struct syscore_ops fw_syscore_ops = {
	.suspend = fw_suspend,
};
#else
static int fw_cache_piggyback_on_request(const char *name)
{
	return 0;
}
#endif

static void __init fw_cache_init(void)
{
	spin_lock_init(&fw_cache.lock);
	INIT_LIST_HEAD(&fw_cache.head);
	fw_cache.state = FW_LOADER_NO_CACHE;

#ifdef CONFIG_PM_SLEEP
	spin_lock_init(&fw_cache.name_lock);
	INIT_LIST_HEAD(&fw_cache.fw_names);
	fw_cache.cnt = 0;

	init_waitqueue_head(&fw_cache.wait_queue);
	INIT_DELAYED_WORK(&fw_cache.work,
			  device_uncache_fw_images_work);

	fw_cache.pm_notify.notifier_call = fw_pm_notify;
	register_pm_notifier(&fw_cache.pm_notify);

	register_syscore_ops(&fw_syscore_ops);
#endif
}

static int __init firmware_class_init(void)
{
	fw_cache_init();
	return class_register(&firmware_class);
}

static void __exit firmware_class_exit(void)
{
#ifdef CONFIG_PM_SLEEP
	unregister_syscore_ops(&fw_syscore_ops);
	unregister_pm_notifier(&fw_cache.pm_notify);
#endif
	class_unregister(&firmware_class);
}

fs_initcall(firmware_class_init);
module_exit(firmware_class_exit);

EXPORT_SYMBOL(release_firmware);
EXPORT_SYMBOL(request_firmware);
EXPORT_SYMBOL(request_firmware_nowait);
EXPORT_SYMBOL_GPL(cache_firmware);
EXPORT_SYMBOL_GPL(uncache_firmware);

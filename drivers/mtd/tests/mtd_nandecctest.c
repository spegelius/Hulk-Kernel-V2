#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/list.h>
#include <linux/random.h>
#include <linux/string.h>
#include <linux/bitops.h>
#include <linux/mtd/nand_ecc.h>

#if defined(CONFIG_MTD_NAND) || defined(CONFIG_MTD_NAND_MODULE)

static void inject_single_bit_error(void *data, size_t size)
{
	unsigned long offset = random32() % (size * BITS_PER_BYTE);

	__change_bit(offset, data);
}

static void dump_data_ecc(void *error_data, void *error_ecc, void *correct_data,
			void *correct_ecc, const size_t size)
{
	pr_info("hexdump of error data:\n");
	print_hex_dump(KERN_INFO, "", DUMP_PREFIX_OFFSET, 16, 4,
			error_data, size, false);
	print_hex_dump(KERN_INFO, "hexdump of error ecc: ",
			DUMP_PREFIX_NONE, 16, 1, error_ecc, 3, false);

	pr_info("hexdump of correct data:\n");
	print_hex_dump(KERN_INFO, "", DUMP_PREFIX_OFFSET, 16, 4,
			correct_data, size, false);
	print_hex_dump(KERN_INFO, "hexdump of correct ecc: ",
			DUMP_PREFIX_NONE, 16, 1, correct_ecc, 3, false);
}

static unsigned char correct_data[512];
static unsigned char error_data[512];

static int nand_ecc_test(const size_t size)
{
	unsigned char correct_ecc[3];
	unsigned char error_ecc[3];
	char testname[30];

	BUG_ON(sizeof(correct_data) < size);

	sprintf(testname, "nand-ecc-%zu", size);

	get_random_bytes(correct_data, size);

	memcpy(error_data, correct_data, size);
	inject_single_bit_error(error_data, size);

	__nand_calculate_ecc(correct_data, size, correct_ecc);
	__nand_calculate_ecc(error_data, size, error_ecc);
	__nand_correct_data(error_data, correct_ecc, error_ecc, size);

	if (!memcmp(correct_data, error_data, size)) {
		pr_info("mtd_nandecctest: ok - %s\n", testname);
		return 0;
	}

	pr_err("mtd_nandecctest: not ok - %s\n", testname);
	dump_data_ecc(error_data, error_ecc, correct_data, correct_ecc, size);

	return -EINVAL;
}

#else

static int nand_ecc_test(const size_t size)
{
	return 0;
}

#endif

static int __init ecc_test_init(void)
{
	int err;

	err = nand_ecc_test(256);
	if (err)
		return err;

	return nand_ecc_test(512);
}

static void __exit ecc_test_exit(void)
{
}

module_init(ecc_test_init);
module_exit(ecc_test_exit);

MODULE_DESCRIPTION("NAND ECC function test module");
MODULE_AUTHOR("Akinobu Mita");
MODULE_LICENSE("GPL");

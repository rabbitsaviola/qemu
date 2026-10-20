/* SF32LB52x peripheral flash address translation. */
/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/misc/sifli-sbus.h"

static void test_flash_windows(void)
{
    g_assert_cmphex(sifli_sbus_to_cpu_addr(0x60000000), ==, 0x10000000);
    g_assert_cmphex(sifli_sbus_to_cpu_addr(0x603fffff), ==, 0x103fffff);
    g_assert_cmphex(sifli_sbus_to_cpu_addr(0x62000000), ==, 0x12000000);
    g_assert_cmphex(sifli_sbus_to_cpu_addr(0x62020000), ==, 0x12020000);
    g_assert_cmphex(sifli_sbus_to_cpu_addr(0x63ffffff), ==, 0x13ffffff);
    g_assert_cmphex(sifli_sbus_to_cpu_addr(0x6fffffff), ==, 0x1fffffff);
}

static void test_other_addresses(void)
{
    g_assert_cmphex(sifli_sbus_to_cpu_addr(0x60400000), ==, 0x60400000);
    g_assert_cmphex(sifli_sbus_to_cpu_addr(0x607fffff), ==, 0x607fffff);
    g_assert_cmphex(sifli_sbus_to_cpu_addr(0x61ffffff), ==, 0x61ffffff);
    g_assert_cmphex(sifli_sbus_to_cpu_addr(0x70000000), ==, 0x70000000);
    g_assert_cmphex(sifli_sbus_to_cpu_addr(0x20000000), ==, 0x20000000);
    g_assert_cmphex(sifli_sbus_to_cpu_addr(0x12020000), ==, 0x12020000);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/sifli/sbus/flash", test_flash_windows);
    g_test_add_func("/sifli/sbus/passthrough", test_other_addresses);
    return g_test_run();
}

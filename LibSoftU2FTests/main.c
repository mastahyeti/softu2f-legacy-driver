//
//  main.c
//  LibSoftU2FTests
//
//  Created by Benjamin P Toews on 1/20/17.
//  Copyright © 2017 GitHub. All rights reserved.
//

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <stdio.h>
#include <unistd.h>
#include "u2f-host.h"
#include "u2f_hid.h"
#include "LibSoftU2FTests.h"

u2fh_devs *devs = NULL;

// Test INIT request/response.
void test_init(void **state) {
  u2fh_rc rc;
  uint8_t resp_bytes[1024];
  size_t resp_len = sizeof(resp_bytes);
  U2FHID_INIT_RESP *resp = (U2FHID_INIT_RESP *)resp_bytes;
  U2FHID_INIT_REQ req = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 };
  u2fdevice *dev = get_device();
  dev->cid = CID_BROADCAST;

  rc = u2fh_sendrecv(devs, dev->id, U2FHID_INIT, (uint8_t *)&req, sizeof(U2FHID_INIT_REQ), resp_bytes, &resp_len);
  assert_string_equal(u2fh_strerror_name(U2FH_OK), u2fh_strerror_name(rc));
  assert_int_equal(sizeof(U2FHID_INIT_RESP), resp_len);
  assert_memory_equal(req.nonce, resp->nonce, 8);
  assert_int_equal(CAPFLAG_WINK, resp->capFlags);
}

// Test basic PING request/response.
void test_ping(void **state) {
  unsigned char data[] = "hello";
  unsigned char resp[1024];
  size_t resplen = sizeof(resp);
  u2fh_sendrecv(devs, 0, U2FHID_PING, data, sizeof(data), resp, &resplen);

  assert_int_equal(sizeof(data), resplen);
  assert_string_equal(data, resp);
}

// Test long PING (message fragmentation).
void test_long_ping(void **state) {
  unsigned char data[] = "9dac044c027bf00e1505b32b19a42053dee08f7a8e971e17e447a86d393745591ab720559cb65b0c";
  unsigned char resp[1024];
  size_t resplen = sizeof(resp);
  u2fh_sendrecv(devs, 0, U2FHID_PING, data, sizeof(data), resp, &resplen);

  assert_int_equal(sizeof(data), resplen);
  assert_string_equal(data, resp);
}

// Test long PING — bitshifting is hard :'(
void test_really_long_ping(void **state) {
    unsigned char data[] = "9dac044c027bf00e1505b32b19a42053dee08f7a8e971e17e447a86d393745591ab720559cb65b0c9dac044c027bf00e1505b32b19a42053dee08f7a8e971e17e447a86d393745591ab720559cb65b0c9dac044c027bf00e1505b32b19a42053dee08f7a8e971e17e447a86d393745591ab720559cb65b0c9dac044c027bf00e1505b32b19a42053dee08f7a8e971e17e447a86d393745591ab720559cb65b0c9dac044c027bf00e1505b32b19a42053dee08f7a8e971e17e447a86d393745591ab720559cb65b0c9dac044c027bf00e1505b32b19a42053dee08f7a8e971e17e447a86d393745591ab720559cb65b0c";
    unsigned char resp[1024];
    size_t resplen = sizeof(resp);
    u2fh_sendrecv(devs, 0, U2FHID_PING, data, sizeof(data), resp, &resplen);

    assert_int_equal(sizeof(data), resplen);
    assert_string_equal(data, resp);
}

// Test LOCK request/response.
void test_lock(void **state) {}

void test_register(void **state) {
  u2fh_register(devs, <#const char *challenge#>, <#const char *origin#>, <#char **response#>, <#u2fh_cmdflags flags#>)
}

int setup(void **state) {
  int rc;
  unsigned int max_dev_idx = 0;

  // Init libu2f-host.
  rc = u2fh_global_init(0); // U2FH_DEBUG for debugging.
  if (rc != U2FH_OK) {
    printf("Error initializing libu2f-host: %s\n", u2fh_strerror(rc));
    return -1;
  }

  rc = u2fh_devs_init(&devs);
  if (rc) {
    printf("Error initializing libu2f-host devs.\n");
    return -1;
  }

  while (u2fh_devs_discover(devs, &max_dev_idx)) {
    u2fh_devs_done(devs);

    rc = u2fh_devs_init(&devs);
    if (rc) {
      printf("Error initializing libu2f-host devs.\n");
      return -1;
    }

    printf("libu2f-host couldn't find soft u2f device. Trying again.\n");
    sleep(1);
  }

  if (max_dev_idx != 0) {
    printf("libu2f-host found multiple devices.\n");
    return -1;
  }

  return 0;
}

int teardown(void **state) {
  if (devs) u2fh_devs_done(devs);
  devs = NULL;

  u2fh_global_done();
  
  return 0;
}

u2fdevice *get_device() {
  u2fdevice *dev = devs->first;

  while (dev) {
    if (dev->id == 0) return dev;
    dev = dev->next;
  }

  assert_true(0);
  return NULL;
}

int main(void) {
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_init),
    cmocka_unit_test(test_ping),
    cmocka_unit_test(test_long_ping),
    cmocka_unit_test(test_really_long_ping),
    cmocka_unit_test(test_lock),
  };

  return cmocka_run_group_tests(tests, setup, teardown);
}

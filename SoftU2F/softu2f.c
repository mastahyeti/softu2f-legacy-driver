//
//  SoftU2FClientInterface.c
//  SoftU2F
//
//  Created by Benjamin P Toews on 1/12/17.
//  Copyright © 2017 GitHub. All rights reserved.
//

#include "softu2f.h"
#include "internal.h"

// Initialize libSoftU2F before usage.
softu2f_ctx *softu2f_init() {
  softu2f_ctx *ctx = NULL;
  io_service_t service = IO_OBJECT_NULL;
  kern_return_t ret;

  service = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching(kSoftU2FDriverClassName));
  if (!service) {
    fprintf(stderr, "SoftU2F.kext not loaded.\n");
    return NULL;
  }

  // Allocate a new context.
  ctx = (softu2f_ctx *)calloc(1, sizeof(softu2f_ctx));

  // Open connection to user client.
  ret = IOServiceOpen(service, mach_task_self(), 0, &ctx->con);
  if (ret != KERN_SUCCESS) {
    fprintf(stderr, "Error connecting to SoftU2F.kext: %d\n", ret);
    goto fail;
  }
  IOObjectRelease(service);
  service = IO_OBJECT_NULL;

  // Initialize connection to user client.
  ret = IOConnectCallScalarMethod(ctx->con, kSoftU2FUserClientOpen, NULL, 0, NULL, NULL);
  if (ret != KERN_SUCCESS) {
    fprintf(stderr, "Unable to open user client: %d.\n", ret);
    goto fail;
  }

  return ctx;

fail:
  if (service) IOObjectRelease(service);
  if (ctx) softu2f_deinit(ctx);
  return NULL;
}

// Shutdown the run loop.
void softu2f_shutdown(softu2f_ctx *ctx) {
    ctx->shutdown = true;
}

// Cleanup after using libSoftU2F.
void softu2f_deinit(softu2f_ctx *ctx) {
  kern_return_t ret;

  // Deinitialize connection to user client.
  ret = IOConnectCallScalarMethod(ctx->con, kSoftU2FUserClientClose, NULL, 0, NULL, NULL);
  if (ret != KERN_SUCCESS) {
    fprintf(stderr, "Unable to close user client: %d.\n", ret);
    return;
  }

  // Close user client connection.
  ret = IOServiceClose(ctx->con);
  if (ret != KERN_SUCCESS) {
    fprintf(stderr, "Error closing connection to SoftU2F.kext: %d.\n", ret);
    return;
  }

  // Cleanup
  free(ctx);
}

// Read HID messages from device in loop.
void softu2f_run(softu2f_ctx *ctx) {
  softu2f_hid_message *hidmsg;
  softu2f_hid_message_handler handler;

  while (true) {
    // Stop the run loop.
    if (ctx->shutdown)
      break;

    hidmsg = softu2f_hid_msg_read(ctx);

    if (hidmsg) {
      handler = softu2f_hid_msg_handler(ctx, hidmsg);

      if (handler) {
        if (!handler(ctx, hidmsg)) {
          fprintf(stderr, "Error handling HID message\n");
        }
      } else {
        fprintf(stderr, "No handler for HID message\n");
        softu2f_hid_err_send(ctx, hidmsg->cid, ERR_OTHER);
      }

      handler = NULL;
    }

    softu2f_hid_msg_free(hidmsg);
  }
}

// Is this client allowed to start a transaction (not locked by another client)?
bool softu2f_hid_is_unlocked_for_client(softu2f_ctx *ctx, uint32_t cid) {
  // No lock.
  if (!ctx->lock)
    return true;

  // Lock expired.
  if (ctx->lock->expiration < time(NULL)) {
    free(ctx->lock);
    ctx->lock = NULL;
    return true;
  }

  // Is it locked by this client.
  return ctx->lock->cid == cid;
}

// Send a HID message to the device.
bool softu2f_hid_msg_send(softu2f_ctx *ctx, softu2f_hid_message *msg) {
  uint8_t *src;
  uint8_t *src_end;
  uint8_t *dst;
  uint8_t *dst_end;
  uint8_t seq;
  U2FHID_FRAME frame;
  kern_return_t ret;

  memset(&frame, 0, HID_RPT_SIZE);

  // Init frame.
  frame.cid = msg->cid;
  frame.type |= TYPE_INIT;
  frame.init.cmd |= msg->cmd;
  frame.init.bcnth = CFDataGetLength(msg->data) << 8;
  frame.init.bcntl = CFDataGetLength(msg->data) & 0xff;

  src = (uint8_t *)CFDataGetBytePtr(msg->data);
  src_end = src + CFDataGetLength(msg->data);
  dst = frame.init.data;
  dst_end = dst + sizeof(frame.init.data);

  while (1) {
    if (src_end - src > dst_end - dst) {
      memcpy(dst, src, dst_end - dst);
      src += dst_end - dst;
    } else {
      memcpy(dst, src, src_end - src);
      src += src_end - src;
    }

    // Send frame.
    debug_frame(&frame, false);
    ret = IOConnectCallStructMethod(ctx->con, kSoftU2FUserClientSendFrame,
                                    &frame, HID_RPT_SIZE, NULL, NULL);
    if (ret != kIOReturnSuccess) {
      fprintf(stderr, "Error calling kSoftU2FUserClientSendFrame: 0x%08x\n",
              ret);
      return false;
    }

    // No more frames.
    if (src >= src_end)
      break;

    // Polling interval is 5ms.
    sleep(0.005);

    // Cont frame.
    dst = frame.cont.data;
    dst_end = dst + sizeof(frame.cont.data);
    frame.cont.seq = seq++;
    memset(frame.cont.data, 0, sizeof(frame.cont.data));
  }

  return true;
}

// Send a HID error to the device.
bool softu2f_hid_err_send(softu2f_ctx *ctx, uint32_t cid, uint8_t code) {
  softu2f_hid_message msg;

  msg.cmd = U2FHID_ERROR;
  msg.cid = cid;
  msg.bcnt = 1;
  msg.data = CFDataCreateWithBytesNoCopy(NULL, &code, 1, NULL);

  return softu2f_hid_msg_send(ctx, &msg);
}

// Read a HID message from the device.
softu2f_hid_message *softu2f_hid_msg_read(softu2f_ctx *ctx) {
  kern_return_t ret;
  softu2f_hid_message *msg;
  U2FHID_FRAME frame;
  unsigned long frame_size = HID_RPT_SIZE;

  msg = (softu2f_hid_message *)calloc(1, sizeof(softu2f_hid_message));
  if (!msg) {
    fprintf(stderr, "No memory for new message.\n");
    return NULL;
  }

  while (1) {
    // Stop the run loop.
    if (ctx->shutdown)
      goto done;

    ret = IOConnectCallStructMethod(ctx->con, kSoftU2FUserClientGetFrame, NULL, 0, &frame, &frame_size);

    switch (ret) {
    case kIOReturnNoFrames:
      // Wait until we're notified by kernel about a new setReport.
      softu2f_wait_for_set_report(ctx);
      break;

    case kIOReturnSuccess:
      if (frame_size != HID_RPT_SIZE) {
        fprintf(stderr, "bad frame\n");
        goto done;
      }

      if (!softu2f_hid_is_unlocked_for_client(ctx, frame.cid)) {
        softu2f_hid_err_send(ctx, frame.cid, ERR_CHANNEL_BUSY);
        break;
      }

      if (!softu2f_hid_msg_frame_read(ctx, msg, &frame)) {
        goto done;
      }

      break;

    default:
      fprintf(stderr, "error calling kSoftU2FUserClientGetFrame: 0x%08x\n",
              ret);
      goto done;
    }

    if (msg->buf) {
      if (CFDataGetLength(msg->buf) == msg->bcnt) {
        msg->data = CFDataCreateCopy(NULL, msg->buf);
        CFRelease(msg->buf);
        msg->buf = NULL;
        return msg;
      }
    }
  }

  return NULL;

done:
  softu2f_hid_msg_free(msg);
  return NULL;
}

// Read an individual HID frame from the device into a HID message.
bool softu2f_hid_msg_frame_read(softu2f_ctx *ctx, softu2f_hid_message *msg,
                                U2FHID_FRAME *frame) {
  uint8_t *data;
  unsigned int ndata;

  debug_frame(frame, true);

  switch (FRAME_TYPE(*frame)) {
  case TYPE_INIT:
    if (frame->init.cmd == U2FHID_SYNC && msg->buf && msg->cid == frame->cid) {
      softu2f_hid_msg_frame_handle_sync(ctx, frame);
      return false;
    }

    if (msg->buf) {
      softu2f_hid_err_send(ctx, frame->cid, ERR_CHANNEL_BUSY);
      fprintf(stderr, "init frame out of order. ignoring.\n");
      return true;
    }

    msg->cmd = frame->init.cmd;
    msg->cid = frame->cid;
    msg->bcnt = MSG_LEN(*frame);
    msg->buf = CFDataCreateMutable(NULL, msg->bcnt);

    data = frame->init.data;

    if (msg->bcnt > sizeof(frame->init.data)) {
      ndata = sizeof(frame->init.data);
    } else {
      ndata = msg->bcnt;
    }

    break;

  case TYPE_CONT:
    if (!msg->buf) {
      fprintf(stderr, "cont frame out of order. ignoring\n");
      return true;
    }

    if (frame->cid != msg->cid) {
      fprintf(stderr, "spurious CNT from other channel. ignoring.\n");
      return true;
    }

    if (FRAME_SEQ(*frame) != msg->lastSeq++) {
      fprintf(stderr, "bad seq in cont frame (%d). bailing\n", FRAME_SEQ(*frame));
      softu2f_hid_err_send(ctx, frame->cid, ERR_INVALID_SEQ);
      return false;
    }

    data = frame->cont.data;

    if (CFDataGetLength(msg->buf) + sizeof(frame->cont.data) > msg->bcnt) {
      ndata = msg->bcnt - (uint16_t)CFDataGetLength(msg->buf);
    } else {
      ndata = sizeof(frame->cont.data);
    }

    break;

  default:
    fprintf(stderr, "unknown frame type: 0x%08x\n", FRAME_TYPE(*frame));
    return false;
  }

  CFDataAppendBytes(msg->buf, data, ndata);

  return true;
}

// Handle a SYNC packet.
bool softu2f_hid_msg_frame_handle_sync(softu2f_ctx *ctx, U2FHID_FRAME *frame) {
  U2FHID_SYNC_REQ *req_data;
  U2FHID_SYNC_RESP resp_data;
  softu2f_hid_message resp;

  req_data = (U2FHID_SYNC_REQ *)frame->init.data;
  resp_data.nonce = req_data->nonce;

  resp.cid = frame->cid;
  resp.cmd = U2FHID_SYNC;
  resp.bcnt = sizeof(U2FHID_SYNC_RESP);
  resp.data =
      CFDataCreateWithBytesNoCopy(NULL, (uint8_t *)&resp_data, resp.bcnt, NULL);

  return softu2f_hid_msg_send(ctx, &resp);
}

// Register a handler for a message type.
void softu2f_hid_msg_handler_register(softu2f_ctx *ctx, uint8_t type,
                                      softu2f_hid_message_handler handler) {
  switch (type) {
  case U2FHID_PING:
    ctx->ping_handler = handler;
    break;
  case U2FHID_MSG:
    ctx->msg_handler = handler;
    break;
  case U2FHID_LOCK:
    ctx->lock_handler = handler;
    break;
  case U2FHID_INIT:
    ctx->init_handler = handler;
    break;
  case U2FHID_WINK:
    ctx->wink_handler = handler;
    break;
  }
}

// Find a message handler for a message.
softu2f_hid_message_handler softu2f_hid_msg_handler(softu2f_ctx *ctx,
                                                    softu2f_hid_message *msg) {
  switch (msg->cmd) {
  case U2FHID_PING:
    if (ctx->ping_handler)
      return ctx->ping_handler;
    break;
  case U2FHID_MSG:
    if (ctx->msg_handler)
      return ctx->msg_handler;
    break;
  case U2FHID_LOCK:
    if (ctx->lock_handler)
      return ctx->lock_handler;
    break;
  case U2FHID_INIT:
    if (ctx->init_handler)
      return ctx->init_handler;
    break;
  case U2FHID_WINK:
    if (ctx->wink_handler)
      return ctx->wink_handler;
    break;
  }

  return softu2f_hid_msg_handler_default(ctx, msg);
}

// Find the default message handler for a message.
softu2f_hid_message_handler
softu2f_hid_msg_handler_default(softu2f_ctx *ctx, softu2f_hid_message *msg) {
  switch (msg->cmd) {
  case U2FHID_PING:
    return softu2f_hid_msg_handle_ping;
  case U2FHID_MSG:
    return NULL;
  case U2FHID_LOCK:
    return softu2f_hid_msg_handle_lock;
  case U2FHID_INIT:
    return softu2f_hid_msg_handle_init;
  case U2FHID_WINK:
    return softu2f_hid_msg_handle_wink;
  default:
    return NULL;
  }
}

// Send an INIT response for a given request.
bool softu2f_hid_msg_handle_init(softu2f_ctx *ctx, softu2f_hid_message *req) {
  softu2f_hid_message resp;
  U2FHID_INIT_RESP resp_data = {0};
  U2FHID_INIT_REQ *req_data;

  req_data = (U2FHID_INIT_REQ *)CFDataGetBytePtr(req->data);

  resp.cmd = U2FHID_INIT;
  resp.bcnt = sizeof(U2FHID_INIT_RESP);
  resp.data = CFDataCreateWithBytesNoCopy(NULL, (uint8_t *)&resp_data, resp.bcnt, NULL);

  if (req->cid == CID_BROADCAST) {
    // Allocate a new CID for the client and tell them about it.
    resp.cid = CID_BROADCAST;
    resp_data.cid = ctx->next_cid++;
  } else {
    // Use whatever CID they wanted.
    resp.cid = req->cid;
    resp_data.cid = req->cid;
  }

  memcpy(resp_data.nonce, req_data->nonce, INIT_NONCE_SIZE);
  resp_data.versionInterface = U2FHID_IF_VERSION;
  resp_data.versionMajor = 0;
  resp_data.versionMinor = 0;
  resp_data.versionBuild = 0;
  resp_data.capFlags |= CAPFLAG_WINK;

  return softu2f_hid_msg_send(ctx, &resp);
}

// Send a PING response for a given request.
bool softu2f_hid_msg_handle_ping(softu2f_ctx *ctx, softu2f_hid_message *req) {
  softu2f_hid_message resp;

  resp.cid = req->cid;
  resp.cmd = U2FHID_PING;
  resp.bcnt = req->bcnt;
  resp.data = req->data;

  return softu2f_hid_msg_send(ctx, &resp);
}

// Send a WINK response for a given request.
bool softu2f_hid_msg_handle_wink(softu2f_ctx *ctx, softu2f_hid_message *req) {
  softu2f_hid_message resp;

  resp.cid = req->cid;
  resp.cmd = U2FHID_WINK;
  resp.bcnt = req->bcnt;
  resp.data = req->data;

  return softu2f_hid_msg_send(ctx, &resp);
}

// Send a LOCK response for a given request.
bool softu2f_hid_msg_handle_lock(softu2f_ctx *ctx, softu2f_hid_message *req) {
  softu2f_hid_message resp;
  uint8_t *duration;
  bool ret;

  duration = (uint8_t *)CFDataGetBytePtr(req->data);
  if (*duration > 10) {
    // Max lock is 10 seconds.
    *duration = 10;
  }

  if (*duration == 0) {
    // Clear lock.
    if (ctx->lock) {
      free(ctx->lock);
      ctx->lock = NULL;
    }
  } else {
    ctx->lock = malloc(sizeof(softu2f_hid_lock));
    ctx->lock->cid = req->cid;
    ctx->lock->expiration = time(NULL) + *duration;
  }

  resp.cid = req->cid;
  resp.cmd = U2FHID_LOCK;
  resp.bcnt = 0;
  resp.data = CFDataCreateMutable(NULL, 0);

  ret = softu2f_hid_msg_send(ctx, &resp);
  CFRelease(resp.data);
  return ret;
}

// Free a HID message and associated data.
void softu2f_hid_msg_free(softu2f_hid_message *msg) {
  if (msg) {
    if (msg->data)
      CFRelease(msg->data);
    if (msg->buf)
      CFRelease(msg->buf);
    free(msg);
  }
}

// Callback called when a setReport is received by the driver.
void _softu2f_async_callback(void *refcon, IOReturn result) {
  // Return execution to main run loop.
  CFRunLoopStop(CFRunLoopGetMain());
}

void _softu2f_timer_callback(CFRunLoopTimerRef timer, void* info) {
  // Return execution to main run loop.
  CFRunLoopStop(CFRunLoopGetMain());
}

// Block until setReport is called on the device.
void softu2f_wait_for_set_report(softu2f_ctx *ctx) {
  IONotificationPortRef notificationPort;
  CFRunLoopSourceRef runLoopSource;
  CFRunLoopTimerRef timer;
  io_async_ref64_t asyncRef;
  kern_return_t ret;

  notificationPort = IONotificationPortCreate(kIOMasterPortDefault);
  runLoopSource = IONotificationPortGetRunLoopSource(notificationPort);
  timer = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + 1, 0, 0, 0, _softu2f_timer_callback, NULL);

  CFRunLoopAddSource(CFRunLoopGetCurrent(), runLoopSource, kCFRunLoopDefaultMode);
  CFRunLoopAddTimer(CFRunLoopGetCurrent(), timer, kCFRunLoopDefaultMode);

  asyncRef[kIOAsyncCalloutFuncIndex] = (uint64_t)_softu2f_async_callback;
  asyncRef[kIOAsyncCalloutRefconIndex] = 0;

  ret = IOConnectCallAsyncScalarMethod(ctx->con, kSoftU2FUserClientNotifyFrame, IONotificationPortGetMachPort(notificationPort), asyncRef, kIOAsyncCalloutCount, NULL, 0, NULL, NULL);
  if (ret != KERN_SUCCESS) {
    fprintf(stderr, "Unable to register notification port: 0x%08x\n", ret);
    goto done;
  }

  CFRunLoopRun();

done:
  CFRunLoopRemoveSource(CFRunLoopGetCurrent(), runLoopSource, kCFRunLoopDefaultMode);
  CFRunLoopRemoveTimer(CFRunLoopGetCurrent(), timer, kCFRunLoopDefaultMode);
  IONotificationPortDestroy(notificationPort);
}

void debug_frame(U2FHID_FRAME *frame, bool recv) {
  uint8_t *data = NULL;
  uint16_t dlen = 0;

  if (recv) {
    fprintf(stderr, "Received frame:\n");
  } else {
    fprintf(stderr, "Sending frame:\n");
  }

  fprintf(stderr, "\tCID: 0x%08x\n", frame->cid);

  switch (FRAME_TYPE(*frame)) {
    case TYPE_INIT:
      fprintf(stderr, "\tTYPE: INIT\n");
      fprintf(stderr, "\tCMD: 0x%02x\n", frame->init.cmd & ~TYPE_MASK);
      fprintf(stderr, "\tBCNTH: 0x%02x\n", frame->init.bcnth);
      fprintf(stderr, "\tBCNTL: 0x%02x\n", frame->init.bcntl);
      data = frame->init.data;
      dlen = HID_RPT_SIZE - 7;

      break;

    case TYPE_CONT:
      fprintf(stderr, "\tTYPE: CONT\n");
      fprintf(stderr, "\tSEQ: 0x%02x\n", frame->cont.seq);
      data = frame->cont.data;
      dlen = HID_RPT_SIZE - 5;

      break;
  }

  fprintf(stderr, "\tDATA:");
  for(int i = 0; i < dlen; i++) {
    fprintf(stderr, " %02x", data[i]);
  }

  fprintf(stderr, "\n\n");
}
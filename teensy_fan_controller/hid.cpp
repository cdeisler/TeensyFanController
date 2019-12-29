//
// Created by jd on 12/15/2019.
//

#include <memory>
#include "hid.h"
#include "core.h"


#ifdef USB_RAWHID_EN


static byte config_bytes[CONFIG_BYTES];

HID::HID(const std::unique_ptr<TempController> &ctrl, RuntimeConfig &config) : ctrl(ctrl), config(config) {
}

uint8_t HID::send() {
  uint8_t n = 0;
  if (state == HID_DATA) {
    // transmit data packet
    setupPayloadData();
    n = RawHID.send(buffer, 100);
  }
  else if (state == HID_CONFIG) {
    // config requested, transmit (3) config packets

    setupPayloadConfig();
    n = RawHID.send(buffer, 100);

    setupPayloadConfigB();
    n += RawHID.send(buffer, 100);

    setupPayloadConfigC();
    setState(HID_DATA);
    n += RawHID.send(buffer, 100);

    if (n < sizeof(buffer) * 3) {
      Serial.println("HID SEND ERROR");
      return 0;
    }
  }
  return n;
}

uint8_t HID::recv() {
  uint8_t n = 0;
  n = RawHID.recv(buffer, 0);  // 0 timeout = do not wait
  if (n > 0) {
    if (buffer[0] == HID_IN_PAYLOAD_REQ_CONFIG1 && buffer[1] == HID_IN_PAYLOAD_REQ_CONFIG2 && state != HID_DOWNLOAD) {
      // Remote requested configuration payload
      setState(HID_CONFIG);  // transmit config in send()
    }
    else if (buffer[0] == HID_PAYLOAD_CONFIG1 && buffer[1] == HID_PAYLOAD_CONFIG2 && state != HID_DOWNLOAD) {
      // Remote is sending configuration payload chunk 1
      setState(HID_DOWNLOAD);  // expecting sequential config payloads
      memset(config_bytes, '\0', CONFIG_BYTES);
      memcpy(config_bytes, (buffer + 2), CHUNK_SIZE);
      logConfigChunk(1);
    }
    else if (buffer[0] == HID_PAYLOAD_CONFIG_B1 && buffer[1] == HID_PAYLOAD_CONFIG_B2 && state == HID_DOWNLOAD) {
      // Remote is sending configuration payload chunk 2
      memcpy((config_bytes + CHUNK_SIZE), (buffer + 2), CHUNK_SIZE);
      logConfigChunk(2);
    }
    else if (buffer[0] == HID_PAYLOAD_CONFIG_C1 && buffer[1] == HID_PAYLOAD_CONFIG_C2 && state == HID_DOWNLOAD) {
      // Remote is sending configuration payload chunk 3
      setState(HID_DATA);
      memcpy((config_bytes + 2 * CHUNK_SIZE), (buffer + 2), CONFIG_BYTES - 2 * CHUNK_SIZE);
      logConfigChunk(3);

      // update config from received bytes
      config = RuntimeConfig::parse_bytes(config_bytes, CONFIG_BYTES);
      ctrl->configChanged();  // trigger hardware/logic updates
    }
    else if (state == HID_DOWNLOAD) {
      Serial.println("Unexpected packet received while in HID_DOWNLOAD state");
      setState(HID_DATA);
    }
  }
  return n;
}

void HID::setState(HID_STATE state) {
  this->state = state;
  Serial.print("HID state=");
  Serial.println((
                   state == HID_CONFIG ? "HID_CONFIG"
                   : state == HID_DOWNLOAD ? "HID_DOWNLOAD"
                   : state == HID_DATA ? "HID_DATA"
                   : "?"
                 ));
}

void HID::logConfigChunk(uint8_t chunk) {
  Serial.print("Config chunk ");
  Serial.print(chunk);
  Serial.println(" downloaded");
}


void HID::setupPayloadConfig() {
  // copy entire configuration into config_bytes
  if (config.to_bytes(config_bytes, CONFIG_BYTES) != 0) {
    return;  // -1;  // TODO errno
  }

  // first 2 bytes are a signature
  buffer[0] = HID_PAYLOAD_CONFIG1;
  buffer[1] = HID_PAYLOAD_CONFIG2;

  // place first chunk (48 bytes) in HID buffer
  memcpy((buffer + 2), config_bytes, CHUNK_SIZE);

  FILL_ZEROS(buffer, (CHUNK_SIZE + 2), CONFIG_BYTES);
  buffer[63] = HID_CONFIG;  // put next state at the end
}

void HID::setupPayloadConfigB() {
  // first 2 bytes are a signature
  buffer[0] = HID_PAYLOAD_CONFIG_B1;
  buffer[1] = HID_PAYLOAD_CONFIG_B2;

  // place 2nd chunk (48 bytes) in HID buffer
  memcpy((buffer + 2), (config_bytes + CHUNK_SIZE), CHUNK_SIZE);

  FILL_ZEROS(buffer, (CHUNK_SIZE + 2), CONFIG_BYTES);
  buffer[63] = HID_CONFIG;  // put next state at the end
}

void HID::setupPayloadConfigC() {
  // first 2 bytes are a signature
  buffer[0] = HID_PAYLOAD_CONFIG_C1;
  buffer[1] = HID_PAYLOAD_CONFIG_C2;

  // place remaining chunk (48 bytes) in HID buffer
  memcpy((buffer + 2), (config_bytes + 2 * CHUNK_SIZE), CONFIG_BYTES - 2 * CHUNK_SIZE);

  FILL_ZEROS(buffer, (CONFIG_BYTES - 2 * CHUNK_SIZE + 2), CONFIG_BYTES);
  buffer[63] = HID_DATA;  // put next state at the end

  // zero config_bytes
  memset(config_bytes, '\0', CONFIG_BYTES);
}

void HID::setupPayloadData() {
  // first 2 bytes are a signature
  buffer[0] = HID_OUT_PAYLOAD_DATA1;
  buffer[1] = HID_OUT_PAYLOAD_DATA2;
  // write values into buffer
  uint64_t val;
  val = (uint64_t) (ctrl->supplyTemp.val * 1000);
  memcpy((buffer + 2), &val, 4);

  val = (uint64_t) (ctrl->returnTemp.val * 1000);
  memcpy((buffer + 6), &val, 4);

  val = (uint64_t) (ctrl->caseTemp.val * 1000);
  memcpy((buffer + 10), &val, 4);

  val = (uint64_t) (ctrl->auxTemp.val * 1000);
  memcpy((buffer + 14), &val, 4);

  val = (uint64_t) (ctrl->getDeltaT() * 1000);
  memcpy((buffer + 18), &val, 4);

  val = (uint64_t) (ctrl->getFanPercentPID() * 1000);
  memcpy((buffer + 22), &val, 4);

  val = (uint64_t) (ctrl->getFanPercentTbl() * 1000);
  memcpy((buffer + 26), &val, 4);

  val = (uint64_t) (ctrl->getTempSetpoint() * 1000);
  memcpy((buffer + 30), &val, 4);

  memcpy((buffer + 34), &ctrl->fan1.rpm, 2);
  memcpy((buffer + 36), &ctrl->fan2.rpm, 2);
  memcpy((buffer + 38), &ctrl->fan3.rpm, 2);
  memcpy((buffer + 40), &ctrl->fan4.rpm, 2);
  memcpy((buffer + 42), &ctrl->fan5.rpm, 2);
  memcpy((buffer + 44), &ctrl->fan6.rpm, 2);

  FILL_ZEROS(buffer, 46, CONFIG_BYTES);
  buffer[63] = HID_DATA;  // put next state at the end
}

#endif

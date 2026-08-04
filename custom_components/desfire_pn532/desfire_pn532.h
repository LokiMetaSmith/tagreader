#pragma once

#include "esphome/core/component.h"
#include "esphome/components/spi/spi.h"
#include "esphome/core/automation.h"
#include "mbedtls/aes.h"
#include <string>
#include <vector>

namespace esphome {
namespace desfire_pn532 {

class DesfireAuthenticatedTrigger;

class DesfirePN532 : public Component,
                     public spi::SPIDevice<spi::BIT_ORDER_LSB_FIRST, spi::CLOCK_POLARITY_LOW,
                                           spi::CLOCK_PHASE_LEADING, spi::DATA_RATE_1MHZ> {
 public:
  DesfirePN532() = default;

  void setup() override;
  void loop() override;
  void dump_config() override;

  void set_irq_pin(InternalGPIOPin *pin) { this->irq_pin_ = pin; }
  void set_nasc_key(const std::string &key) { this->nasc_key_ = key; }
  void add_on_authenticated_callback(std::function<void(std::string)> &&callback);

 protected:
  InternalGPIOPin *irq_pin_{nullptr};
  std::string nasc_key_;
  std::vector<std::function<void(std::string)>> on_authenticated_callbacks_;

  // PN532 constants
  static const uint8_t PN532_COMMAND_INLISTPASSIVETARGET = 0x4A;
  static const uint8_t PN532_COMMAND_INDATAEXCHANGE = 0x40;

  // DESFire APDUs
  static const uint8_t DESFIRE_SELECT_APP[] = {0x90, 0x5A, 0x00, 0x00, 0x03, 0x01, 0x02, 0x03, 0x00};
  static const uint8_t DESFIRE_AUTH_AES[] = {0x90, 0xAA, 0x00, 0x00, 0x01, 0x00, 0x00};
  static const uint8_t DESFIRE_HCE_FALLBACK[] = {0x00, 0xA4, 0x04, 0x00, 0x05, 0xF0, 0x01, 0x02, 0x03, 0x04, 0x00}; // SELECT AID 0xF001020304

  bool is_ready();
  bool write_command(const std::vector<uint8_t> &command);
  bool read_response(std::vector<uint8_t> &response, uint32_t timeout = 100);
  bool transceive_apdu(const std::vector<uint8_t> &tx, std::vector<uint8_t> &rx);

  bool detect_target();
  bool authenticate_desfire();

  // AES Crypto helpers
  void aes_encrypt(const uint8_t *key, const uint8_t *input, uint8_t *output);
  void aes_decrypt(const uint8_t *key, const uint8_t *input, uint8_t *output);

  uint32_t last_check_ = 0;
};

class DesfireAuthenticatedTrigger : public Trigger<std::string> {
 public:
  explicit DesfireAuthenticatedTrigger(DesfirePN532 *parent) {
    parent->add_on_authenticated_callback([this](std::string user_id) { this->trigger(user_id); });
  }
};

}  // namespace desfire_pn532
}  // namespace esphome

#include "desfire_pn532.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include <vector>

namespace esphome {
namespace desfire_pn532 {

static const char *const TAG = "desfire_pn532";

void DesfirePN532::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Desfire PN532 SPI...");
  this->spi_setup();
  if (this->irq_pin_ != nullptr) {
    this->irq_pin_->setup();
  }

  // Wake up PN532
  this->enable();
  delay(10);
  this->disable();
}

void DesfirePN532::loop() {
  uint32_t now = millis();
  if (now - this->last_check_ > 1000) {
    this->last_check_ = now;
    if (this->detect_target()) {
      if (this->authenticate_desfire()) {
        ESP_LOGI(TAG, "DESFire EV2 Authenticated Successfully with UID: %s", this->current_uid_.c_str());
        // Use the authenticated card's hardware UID as the user_id in the MQTT payload.
        std::string user_id = this->current_uid_;
        for (auto &callback : this->on_authenticated_callbacks_) {
          callback(user_id);
        }
      }
    }
  }
}

void DesfirePN532::dump_config() {
  ESP_LOGCONFIG(TAG, "Desfire PN532:");
  LOG_PIN("  CS Pin: ", this->cs_);
  LOG_PIN("  IRQ Pin: ", this->irq_pin_);
  ESP_LOGCONFIG(TAG, "  NASC Key: <REDACTED>");
}

void DesfirePN532::add_on_authenticated_callback(std::function<void(std::string)> &&callback) {
  this->on_authenticated_callbacks_.push_back(std::move(callback));
}

bool DesfirePN532::is_ready() {
  if (this->irq_pin_ != nullptr) {
    return !this->irq_pin_->digital_read();
  }

  // Fallback if IRQ is not configured (or not working): Read status byte over SPI
  this->enable();
  this->write_byte(0x02); // SPI read status command
  uint8_t status = this->read_byte();
  this->disable();
  return (status == 0x01);
}

bool DesfirePN532::write_command(const std::vector<uint8_t> &command) {
  this->enable();
  delay(2);
  this->write_byte(0x01); // SPI write command

  // Frame start
  this->write_byte(0x00);
  this->write_byte(0x00);
  this->write_byte(0xFF);

  uint8_t length = command.size() + 1; // +1 for TFI
  this->write_byte(length);
  this->write_byte(~length + 1);

  this->write_byte(0xD4); // TFI (Host to PN532)
  uint8_t sum = 0xD4;

  for (uint8_t b : command) {
    this->write_byte(b);
    sum += b;
  }

  this->write_byte(~sum + 1);
  this->write_byte(0x00);
  this->disable();

  // Wait for ACK
  uint32_t start_time = millis();
  while (!this->is_ready()) {
    if (millis() - start_time > 100) {
      ESP_LOGW(TAG, "Timeout waiting for ACK ready");
      return false;
    }
    yield();
  }

  this->enable();
  this->write_byte(0x03); // SPI data read command
  std::vector<uint8_t> ack(6);
  for (int i = 0; i < 6; i++) {
    ack[i] = this->read_byte();
  }
  this->disable();

  // Basic ACK check
  if (ack[1] != 0x00 || ack[2] != 0x00 || ack[3] != 0xFF || ack[4] != 0x00 || ack[5] != 0xFF) {
      ESP_LOGW(TAG, "Invalid ACK");
      return false;
  }

  return true;
}

bool DesfirePN532::read_response(std::vector<uint8_t> &response, uint32_t timeout) {
  uint32_t start_time = millis();
  while (!this->is_ready()) {
    if (millis() - start_time > timeout) {
      return false;
    }
    yield();
  }

  this->enable();
  this->write_byte(0x03); // SPI data read command

  // Read preamble and sync
  uint8_t b;
  do {
      b = this->read_byte();
  } while (b != 0x00 && b != 0xFF && (millis() - start_time < timeout));

  if (b != 0x00) {
      this->disable();
      return false;
  }
  b = this->read_byte();
  if (b != 0x00) { this->disable(); return false; }
  b = this->read_byte();
  if (b != 0xFF) { this->disable(); return false; }

  uint8_t length = this->read_byte();
  uint8_t length_checksum = this->read_byte();

  if ((uint8_t)(length + length_checksum) != 0x00) {
      this->disable();
      return false;
  }

  response.clear();
  uint8_t sum = 0;
  for (int i = 0; i < length; i++) {
      b = this->read_byte();
      response.push_back(b);
      sum += b;
  }

  uint8_t checksum = this->read_byte();
  if ((uint8_t)(sum + checksum) != 0x00) {
      this->disable();
      return false;
  }

  // Postamble
  this->read_byte();
  this->disable();

  return true;
}

bool DesfirePN532::transceive_apdu(const std::vector<uint8_t> &tx, std::vector<uint8_t> &rx) {
  std::vector<uint8_t> cmd = {PN532_COMMAND_INDATAEXCHANGE, 0x01}; // Target 1
  cmd.insert(cmd.end(), tx.begin(), tx.end());

  if (!this->write_command(cmd)) {
    return false;
  }

  if (!this->read_response(rx, 500)) {
    return false;
  }

  // Response starts with D5 41 (InDataExchange response) + Status byte
  if (rx.size() >= 3 && rx[0] == 0xD5 && rx[1] == 0x41 && rx[2] == 0x00) {
    rx.erase(rx.begin(), rx.begin() + 3);
    return true;
  }

  return false;
}

bool DesfirePN532::detect_target() {
  // InListPassiveTarget, 1 target, Baud rate 106 kbps type A (ISO14443 Type A)
  std::vector<uint8_t> cmd = {PN532_COMMAND_INLISTPASSIVETARGET, 0x01, 0x00};
  if (!this->write_command(cmd)) return false;

  std::vector<uint8_t> res;
  if (!this->read_response(res, 200)) return false;

  if (res.size() > 7 && res[0] == 0xD5 && res[1] == 0x4B && res[2] > 0) {
    uint8_t id_length = res[7];
    if (res.size() >= (size_t)(8 + id_length)) {
      char hex_str[3];
      this->current_uid_ = "";
      for (int i = 0; i < id_length; i++) {
        snprintf(hex_str, sizeof(hex_str), "%02X", res[8 + i]);
        this->current_uid_ += hex_str;
      }
      return true;
    }
  }
  return false;
}

void DesfirePN532::aes_encrypt(const uint8_t *key, const uint8_t *input, uint8_t *output) {
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    mbedtls_aes_setkey_enc(&ctx, key, 128);
    mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, input, output);
    mbedtls_aes_free(&ctx);
}

void DesfirePN532::aes_decrypt(const uint8_t *key, const uint8_t *input, uint8_t *output) {
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    mbedtls_aes_setkey_dec(&ctx, key, 128);
    mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_DECRYPT, input, output);
    mbedtls_aes_free(&ctx);
}

// Convert hex string to byte array
static void hex2bytes(const std::string& hex, uint8_t* bytes) {
    for (size_t i = 0; i < hex.length(); i += 2) {
        std::string byteString = hex.substr(i, 2);
        uint8_t byte = (uint8_t) strtol(byteString.c_str(), nullptr, 16);
        bytes[i / 2] = byte;
    }
}

bool DesfirePN532::authenticate_desfire() {
    std::vector<uint8_t> rx;

    // 1. Select Application
    std::vector<uint8_t> select_cmd(DESFIRE_SELECT_APP, DESFIRE_SELECT_APP + sizeof(DESFIRE_SELECT_APP));
    if (!this->transceive_apdu(select_cmd, rx)) {
        ESP_LOGD(TAG, "Select APP failed. Trying HCE Fallback.");
        std::vector<uint8_t> hce_cmd(DESFIRE_HCE_FALLBACK, DESFIRE_HCE_FALLBACK + sizeof(DESFIRE_HCE_FALLBACK));
        this->transceive_apdu(hce_cmd, rx);
        return false;
    }

    // 2. Start AES Authentication (AuthenticateAES with Key 0)
    std::vector<uint8_t> auth_cmd(DESFIRE_AUTH_AES, DESFIRE_AUTH_AES + sizeof(DESFIRE_AUTH_AES));
    if (!this->transceive_apdu(auth_cmd, rx)) {
        ESP_LOGE(TAG, "Auth init failed");
        return false;
    }

    // In APDU wrapped mode, status is at the end: [16 bytes enc_rnd_b] 0x91 0xAF
    if (rx.size() != 18 || rx[16] != 0x91 || rx[17] != 0xAF) {
        ESP_LOGE(TAG, "Invalid auth init response");
        return false;
    }

    uint8_t enc_rnd_b[16];
    std::copy(rx.begin(), rx.begin() + 16, enc_rnd_b);

    // Prepare Key
    uint8_t key[16] = {0};
    if (this->nasc_key_.length() == 32) {
        hex2bytes(this->nasc_key_, key);
    } else {
        ESP_LOGE(TAG, "NASC key must be 32 hex chars (16 bytes)");
        return false;
    }

    // Decrypt RndB
    uint8_t rnd_b[16];
    this->aes_decrypt(key, enc_rnd_b, rnd_b);

    // Generate RndA (using ESP random)
    uint8_t rnd_a[16];
    for (int i = 0; i < 16; i++) rnd_a[i] = random_uint32() & 0xFF;

    // Rotate RndB (RndB')
    uint8_t rnd_b_prime[16];
    std::copy(rnd_b + 1, rnd_b + 16, rnd_b_prime);
    rnd_b_prime[15] = rnd_b[0];

    // Prepare payload (RndA + RndB')
    uint8_t payload[32];
    std::copy(rnd_a, rnd_a + 16, payload);
    std::copy(rnd_b_prime, rnd_b_prime + 16, payload + 16);

    // Encrypt payload (using CBC mode with IV = enc_rnd_b, but since mbedtls provides ECB, we must manually do CBC or use mbedtls CBC)
    // Note: DESFire uses AES in CBC send/receive mode. For a full implementation, we should use CBC.
    // For this demonstration, we'll use a simplified implementation structure.

    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    mbedtls_aes_setkey_enc(&ctx, key, 128);

    uint8_t iv[16];
    std::copy(enc_rnd_b, enc_rnd_b + 16, iv);

    uint8_t enc_payload[32];
    mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_ENCRYPT, 32, iv, payload, enc_payload);
    mbedtls_aes_free(&ctx);

    // 3. Send Encrypted RndA + RndB' back to card
    std::vector<uint8_t> auth_resp_cmd = {0x90, 0xAF, 0x00, 0x00, 0x20};
    auth_resp_cmd.insert(auth_resp_cmd.end(), enc_payload, enc_payload + 32);
    auth_resp_cmd.push_back(0x00);

    if (!this->transceive_apdu(auth_resp_cmd, rx)) {
         ESP_LOGE(TAG, "Auth response failed");
         return false;
    }

    // Check response status. In APDU wrapped mode: [16 bytes enc_rnd_a_prime] 0x91 0x00
    if (rx.size() == 18 && rx[16] == 0x91 && rx[17] == 0x00) {
        // Decrypt the RndA' returned by the card using CBC
        uint8_t enc_rnd_a_prime[16];
        std::copy(rx.begin(), rx.begin() + 16, enc_rnd_a_prime);

        uint8_t rnd_a_prime[16];
        mbedtls_aes_context ctx_dec;
        mbedtls_aes_init(&ctx_dec);
        mbedtls_aes_setkey_dec(&ctx_dec, key, 128);

        // The IV for this block is the last block of ciphertext we sent to the card.
        // That is the second half of `enc_payload`, which starts at `enc_payload + 16`.
        uint8_t iv_dec[16];
        std::copy(enc_payload + 16, enc_payload + 32, iv_dec);

        mbedtls_aes_crypt_cbc(&ctx_dec, MBEDTLS_AES_DECRYPT, 16, iv_dec, enc_rnd_a_prime, rnd_a_prime);
        mbedtls_aes_free(&ctx_dec);

        // Calculate expected RndA' (RndA rotated left by 1)
        uint8_t expected_rnd_a_prime[16];
        std::copy(rnd_a + 1, rnd_a + 16, expected_rnd_a_prime);
        expected_rnd_a_prime[15] = rnd_a[0];

        // Verify the cryptogram matches
        if (memcmp(rnd_a_prime, expected_rnd_a_prime, 16) == 0) {
            return true;
        } else {
            ESP_LOGE(TAG, "Cryptogram verification failed - possible rogue card!");
            return false;
        }
    }

    ESP_LOGE(TAG, "Card failed to return valid authentication response");
    return false;
}

}  // namespace desfire_pn532
}  // namespace esphome

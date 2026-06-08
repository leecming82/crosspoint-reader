#include "HalFrontlight.h"

#include <Logging.h>

#include "HalGPIO.h"

HalFrontlight halFrontlight;

namespace {
constexpr uint32_t FRONTLIGHT_PWM_HZ = 1000;
constexpr uint8_t FRONTLIGHT_PWM_BITS = 8;
constexpr uint8_t FRONTLIGHT_COOL_CHANNEL = 4;
constexpr uint8_t FRONTLIGHT_WARM_CHANNEL = 5;
}  // namespace

uint8_t HalFrontlight::pinFor(const Channel channel) const {
  return channel == Channel::Cool ? MURPHY_COOL_PIN : MURPHY_WARM_PIN;
}

void HalFrontlight::begin() {
  if (begun) return;
  begun = true;

#ifdef CROSSPOINT_BOARD_MURPHY_M4
  pinMode(MURPHY_COOL_PIN, OUTPUT);
  pinMode(MURPHY_WARM_PIN, OUTPUT);
  digitalWrite(MURPHY_COOL_PIN, LOW);
  digitalWrite(MURPHY_WARM_PIN, LOW);

  const bool coolOk = ledcAttachChannel(MURPHY_COOL_PIN, FRONTLIGHT_PWM_HZ, FRONTLIGHT_PWM_BITS, FRONTLIGHT_COOL_CHANNEL);
  const bool warmOk = ledcAttachChannel(MURPHY_WARM_PIN, FRONTLIGHT_PWM_HZ, FRONTLIGHT_PWM_BITS, FRONTLIGHT_WARM_CHANNEL);
  available = coolOk && warmOk;
  off();
  LOG_INF("FNL", "Murphy frontlight init cool=GPIO%d warm=GPIO%d ok=%d", MURPHY_COOL_PIN, MURPHY_WARM_PIN,
          available);
#else
  available = false;
#endif
}

uint8_t HalFrontlight::getRaw(const Channel channel) const { return channel == Channel::Cool ? coolDuty : warmDuty; }

void HalFrontlight::set(const uint8_t cool, const uint8_t warm) {
  setRaw(Channel::Cool, cool);
  setRaw(Channel::Warm, warm);
}

void HalFrontlight::setRaw(const Channel channel, const uint8_t duty) {
  begin();
  if (!available) return;

  const uint8_t pin = pinFor(channel);
  ledcWrite(pin, duty);
  if (channel == Channel::Cool) {
    coolDuty = duty;
  } else {
    warmDuty = duty;
  }
  LOG_DBG("FNL", "Frontlight set pin=GPIO%d duty=%u", pin, duty);
}

void HalFrontlight::off() {
#ifdef CROSSPOINT_BOARD_MURPHY_M4
  if (available) {
    ledcWrite(MURPHY_COOL_PIN, 0);
    ledcWrite(MURPHY_WARM_PIN, 0);
  } else {
    digitalWrite(MURPHY_COOL_PIN, LOW);
    digitalWrite(MURPHY_WARM_PIN, LOW);
  }
#endif
  coolDuty = 0;
  warmDuty = 0;
}

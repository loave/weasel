#include "stdafx.h"
#include "Deserializer.h"
#include "Configurator.h"

using namespace weasel;

Deserializer::Ptr Configurator::Create(ResponseParser* pTarget) {
  return Deserializer::Ptr(new Configurator(pTarget));
}

Configurator::Configurator(ResponseParser* pTarget) : Deserializer(pTarget) {}

Configurator::~Configurator() {}

void Configurator::Store(Deserializer::KeyType const& key,
                         std::wstring const& value) {
  if (!m_pTarget->p_context || key.size() < 2)
    return;
  bool bool_value = (!value.empty() && value != L"0");
  if (key[1] == L"inline_preedit") {
    m_pTarget->p_config->inline_preedit = bool_value;
  } else if (key[1] == L"cursor_back") {
    m_pTarget->p_config->cursor_back = _wtoi(value.c_str());
  } else if (key[1] == L"cursor_back_wait_ms") {
    m_pTarget->p_config->cursor_back_wait_ms = _wtoi(value.c_str());
  } else if (key[1] == L"cursor_back_inject") {
    m_pTarget->p_config->cursor_back_inject = bool_value;
  } else if (key[1] == L"cursor_back_inject_only") {
    m_pTarget->p_config->cursor_back_inject_only = bool_value;
  } else if (key[1] == L"cursor_back_delay_ms") {
    m_pTarget->p_config->cursor_back_delay_ms = _wtoi(value.c_str());
  }
}

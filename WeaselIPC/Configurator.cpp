#include "stdafx.h"
#include "Deserializer.h"
#include "Configurator.h"
#include <windows.h>
#include <string>

using namespace weasel;

Deserializer::Ptr Configurator::Create(ResponseParser* pTarget) {
  return Deserializer::Ptr(new Configurator(pTarget));
}

Configurator::Configurator(ResponseParser* pTarget) : Deserializer(pTarget) {}

Configurator::~Configurator() {}

void Configurator::Store(Deserializer::KeyType const& key,
                         std::wstring const& value) {
  // [auto_pair] temp debug: log every config key
  {
    std::string k;
    for (size_t i = 0; i < key.size(); i++) {
      if (i)
        k += ".";
      for (wchar_t c : key[i])
        k += (char)(c < 128 ? c : '?');
    }
    std::string v;
    for (wchar_t c : value)
      v += (char)(c < 128 ? c : '?');
    OutputDebugStringA(
        ("[V02][Configurator] key=" + k + " value=" + v + "\n").c_str());
  }
  if (!m_pTarget->p_context || key.size() < 2)
    return;
  bool bool_value = (!value.empty() && value != L"0");
  if (key[1] == L"inline_preedit") {
    m_pTarget->p_config->inline_preedit = bool_value;
  } else if (key[1] == L"cursor_back") {
    m_pTarget->p_config->cursor_back = _wtoi(value.c_str());
    OutputDebugStringA(("[V02][Configurator] cursor_back set to " +
                        std::to_string(m_pTarget->p_config->cursor_back) + "\n")
                           .c_str());
  }
}

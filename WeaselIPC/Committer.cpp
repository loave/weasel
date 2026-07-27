#include "stdafx.h"
#include "Deserializer.h"
#include "Committer.h"
#include <WeaselUtility.h>

using namespace weasel;

Deserializer::Ptr Committer::Create(ResponseParser* pTarget) {
  return Deserializer::Ptr(new Committer(pTarget));
}

Committer::Committer(ResponseParser* pTarget) : Deserializer(pTarget) {}

Committer::~Committer() {}

void Committer::Store(Deserializer::KeyType const& key,
                      std::wstring const& value) {
  if (!m_pTarget->p_commit)
    return;
  if (key.size() == 1) {
    *m_pTarget->p_commit = unescape_string(value);
  } else if (key.size() == 2 && key[1] == L"cursor_back_count") {
    m_pTarget->p_config->cursor_back_count = std::stoi(value);
  }
}

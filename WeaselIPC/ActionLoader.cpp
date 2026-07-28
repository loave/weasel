#include "stdafx.h"
#include <StringAlgorithm.hpp>
#include "Deserializer.h"
#include "ActionLoader.h"
#include <algorithm>
#include <windows.h>
#include <string>

using namespace weasel;

Deserializer::Ptr ActionLoader::Create(ResponseParser* pTarget) {
  return Deserializer::Ptr(new ActionLoader(pTarget));
}

ActionLoader::ActionLoader(ResponseParser* pTarget) : Deserializer(pTarget) {}

ActionLoader::~ActionLoader() {}

void ActionLoader::Store(Deserializer::KeyType const& key,
                         std::wstring const& value) {
  if (key.size() == 1)  // no extention parts
  {
    // split value by L","
    std::vector<std::wstring> vecAction;
    split(vecAction, value, L",");

    // require specified action deserializers
    std::for_each(vecAction.begin(), vecAction.end(),
                  [this](std::wstring& action) {
                    bool ok = Deserializer::Require(action, m_pTarget);
                    // [auto_pair] temp debug
                    std::string a;
                    for (wchar_t c : action)
                      a += (char)(c < 128 ? c : '?');
                    OutputDebugStringA(("[V05][ActionLoader] require '" + a +
                                        "' -> " + (ok ? "OK" : "FAIL") + "\n")
                                           .c_str());
                  });
  }
}

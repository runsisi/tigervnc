/* Copyright (C) 2002-2005 RealVNC Ltd.  All Rights Reserved.
 * 
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this software; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 * USA.
 */

// -=- Registry.cxx

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <rfb_win32/Registry.h>
#include <rfb_win32/Security.h>
#include <rdr/MemOutStream.h>
#include <rdr/HexOutStream.h>
#include <rdr/HexInStream.h>
#include <stdlib.h>
#include <rfb/LogWriter.h>

// These flags are required to control access control inheritance,
// but are not defined by VC6's headers.  These definitions comes
// from the Microsoft Platform SDK.
#ifndef PROTECTED_DACL_SECURITY_INFORMATION
#define PROTECTED_DACL_SECURITY_INFORMATION     (0x80000000L)
#endif
#ifndef UNPROTECTED_DACL_SECURITY_INFORMATION
#define UNPROTECTED_DACL_SECURITY_INFORMATION     (0x20000000L)
#endif


using namespace rfb;
using namespace rfb::win32;


static LogWriter vlog("Registry");


RegKey::RegKey() : key(0), freeKey(false), valueNameBufLen(0) {}

RegKey::RegKey(const HKEY k) : key(0), freeKey(false), valueNameBufLen(0) {
  LONG result = RegOpenKeyEx(k, 0, 0, KEY_ALL_ACCESS, &key);
  if (result != ERROR_SUCCESS)
    throw rdr::SystemException("RegOpenKeyEx(HKEY)", result);
  vlog.debug("duplicated %p to %p", k, key);
  freeKey = true;
}

RegKey::RegKey(const RegKey& k) : key(0), freeKey(false), valueNameBufLen(0) {
  LONG result = RegOpenKeyEx(k.key, 0, 0, KEY_ALL_ACCESS, &key);
  if (result != ERROR_SUCCESS)
    throw rdr::SystemException("RegOpenKeyEx(RegKey&)", result);
  vlog.debug("duplicated %p to %p", k.key, key);
  freeKey = true;
}

RegKey::~RegKey() {
  close();
}


void RegKey::setHKEY(HKEY k, bool fK) {
  vlog.debug("setHKEY(%p,%d)", k, (int)fK);
  close();
  freeKey = fK;
  key = k;
}


bool RegKey::createKey(const RegKey& root, const char* name) {
  close();
  LONG result = RegCreateKey(root.key, name, &key);
  if (result != ERROR_SUCCESS) {
    vlog.error("RegCreateKey(%p, %s): %lx", root.key, name, result);
    throw rdr::SystemException("RegCreateKeyEx", result);
  }
  vlog.debug("createKey(%p,%s) = %p", root.key, name, key);
  freeKey = true;
  return true;
}

void RegKey::openKey(const RegKey& root, const char* name, bool readOnly) {
  close();
  LONG result = RegOpenKeyEx(root.key, name, 0, readOnly ? KEY_READ : KEY_ALL_ACCESS, &key);
  if (result != ERROR_SUCCESS)
    throw rdr::SystemException("RegOpenKeyEx (open)", result);
  vlog.debug("openKey(%p,%s,%s) = %p", root.key, name,
	         readOnly ? "ro" : "rw", key);
  freeKey = true;
}

void RegKey::setDACL(const PACL acl, bool inherit) {
  DWORD result;
  if ((result = SetSecurityInfo(key, SE_REGISTRY_KEY,
    DACL_SECURITY_INFORMATION |
    (inherit ? UNPROTECTED_DACL_SECURITY_INFORMATION : PROTECTED_DACL_SECURITY_INFORMATION),
    0, 0, acl, 0)) != ERROR_SUCCESS)
    throw rdr::SystemException("RegKey::setDACL failed", result);
}

void RegKey::close() {
  if (freeKey) {
    vlog.debug("RegCloseKey(%p)", key);
    RegCloseKey(key);
    key = 0;
  }
}

void RegKey::deleteKey(const char* name) const {
  LONG result = RegDeleteKey(key, name);
  if (result != ERROR_SUCCESS)
    throw rdr::SystemException("RegDeleteKey", result);
}

void RegKey::deleteValue(const char* name) const {
  LONG result = RegDeleteValue(key, name);
  if (result != ERROR_SUCCESS)
    throw rdr::SystemException("RegDeleteValue", result);
}

void RegKey::awaitChange(bool watchSubTree, DWORD filter, HANDLE event) const {
  LONG result = RegNotifyChangeKeyValue(key, watchSubTree, filter, event, event != 0);
  if (result != ERROR_SUCCESS)
    throw rdr::SystemException("RegNotifyChangeKeyValue", result);
}


RegKey::operator HKEY() const {return key;}


void RegKey::setExpandString(const char* valname, const char* value) const {
  LONG result = RegSetValueEx(key, valname, 0, REG_EXPAND_SZ, (const BYTE*)value, (strlen(value)+1)*sizeof(char));
  if (result != ERROR_SUCCESS) throw rdr::SystemException("setExpandString", result);
}

void RegKey::setString(const char* valname, const char* value) const {
  LONG result = RegSetValueEx(key, valname, 0, REG_SZ, (const BYTE*)value, (strlen(value)+1)*sizeof(char));
  if (result != ERROR_SUCCESS) throw rdr::SystemException("setString", result);
}

void RegKey::setBinary(const char* valname, const void* value, size_t length) const {
  LONG result = RegSetValueEx(key, valname, 0, REG_BINARY, (const BYTE*)value, length);
  if (result != ERROR_SUCCESS) throw rdr::SystemException("setBinary", result);
}

void RegKey::setInt(const char* valname, int value) const {
  LONG result = RegSetValueEx(key, valname, 0, REG_DWORD, (const BYTE*)&value, sizeof(value));
  if (result != ERROR_SUCCESS) throw rdr::SystemException("setInt", result);
}

void RegKey::setBool(const char* valname, bool value) const {
  setInt(valname, value ? 1 : 0);
}

char* RegKey::getString(const char* valname) const {return getRepresentation(valname);}
char* RegKey::getString(const char* valname, const char* def) const {
  try {
    return getString(valname);
  } catch(rdr::Exception&) {
    return strDup(def);
  }
}

std::vector<uint8_t> RegKey::getBinary(const char* valname) const {
  CharArray hex(getRepresentation(valname));
  return hexToBin(hex.buf, strlen(hex.buf));
}
std::vector<uint8_t> RegKey::getBinary(const char* valname, const uint8_t* def, size_t deflen) const {
  try {
    return getBinary(valname);
  } catch(rdr::Exception&) {
    std::vector<uint8_t> out(deflen);
    memcpy(out.data(), def, deflen);
    return out;
  }
}

int RegKey::getInt(const char* valname) const {
  CharArray tmp(getRepresentation(valname));
  return atoi(tmp.buf);
}
int RegKey::getInt(const char* valname, int def) const {
  try {
    return getInt(valname);
  } catch(rdr::Exception&) {
    return def;
  }
}

bool RegKey::getBool(const char* valname) const {
  return getInt(valname) > 0;
}
bool RegKey::getBool(const char* valname, bool def) const {
  return getInt(valname, def ? 1 : 0) > 0;
}

static inline char* terminateData(char* data, int length)
{
  // We must terminate the string, just to be sure.  Stupid Win32...
  int len = length/sizeof(char);
  CharArray str(len+1);
  memcpy(str.buf, data, length);
  str.buf[len] = 0;
  return str.takeBuf();
}

char* RegKey::getRepresentation(const char* valname) const {
  DWORD type, length;
  LONG result = RegQueryValueEx(key, valname, 0, &type, 0, &length);
  if (result != ERROR_SUCCESS)
    throw rdr::SystemException("get registry value length", result);
  CharArray data(length);
  result = RegQueryValueEx(key, valname, 0, &type, (BYTE*)data.buf, &length);
  if (result != ERROR_SUCCESS)
    throw rdr::SystemException("get registry value", result);

  switch (type) {
  case REG_BINARY:
    {
      CharArray hex(binToHex((const uint8_t*)data.buf, length));
      return hex.takeBuf();
    }
  case REG_SZ:
    if (length) {
      return terminateData(data.buf, length);
    } else {
      return strDup("");
    }
  case REG_DWORD:
    {
      CharArray tmp(16);
      sprintf(tmp.buf, "%lu", *((DWORD*)data.buf));
      return tmp.takeBuf();
    }
  case REG_EXPAND_SZ:
    {
    if (length) {
      CharArray str(terminateData(data.buf, length));
      DWORD required = ExpandEnvironmentStrings(str.buf, 0, 0);
      if (required==0)
        throw rdr::SystemException("ExpandEnvironmentStrings", GetLastError());
      CharArray result(required);
      length = ExpandEnvironmentStrings(str.buf, result.buf, required);
      if (required<length)
        throw rdr::Exception("unable to expand environment strings");
      return result.takeBuf();
    } else {
      return strDup("");
    }
    }
  default:
    throw rdr::Exception("unsupported registry type");
  }
}

bool RegKey::isValue(const char* valname) const {
  try {
    CharArray tmp(getRepresentation(valname));
    return true;
  } catch(rdr::Exception&) {
    return false;
  }
}

const char* RegKey::getValueName(int i) {
  DWORD maxValueNameLen;
  LONG result = RegQueryInfoKey(key, 0, 0, 0, 0, 0, 0, 0, &maxValueNameLen, 0, 0, 0);
  if (result != ERROR_SUCCESS)
    throw rdr::SystemException("RegQueryInfoKey", result);
  if (valueNameBufLen < maxValueNameLen + 1) {
    valueNameBufLen = maxValueNameLen + 1;
    delete [] valueName.buf;
    valueName.buf = new char[valueNameBufLen];
  }
  DWORD length = valueNameBufLen;
  result = RegEnumValue(key, i, valueName.buf, &length, NULL, 0, 0, 0);
  if (result == ERROR_NO_MORE_ITEMS) return 0;
  if (result != ERROR_SUCCESS)
    throw rdr::SystemException("RegEnumValue", result);
  return valueName.buf;
}

const char* RegKey::getKeyName(int i) {
  DWORD maxValueNameLen;
  LONG result = RegQueryInfoKey(key, 0, 0, 0, 0, &maxValueNameLen, 0, 0, 0, 0, 0, 0);
  if (result != ERROR_SUCCESS)
    throw rdr::SystemException("RegQueryInfoKey", result);
  if (valueNameBufLen < maxValueNameLen + 1) {
    valueNameBufLen = maxValueNameLen + 1;
    delete [] valueName.buf;
    valueName.buf = new char[valueNameBufLen];
  }
  DWORD length = valueNameBufLen;
  result = RegEnumKeyEx(key, i, valueName.buf, &length, NULL, 0, 0, 0);
  if (result == ERROR_NO_MORE_ITEMS) return 0;
  if (result != ERROR_SUCCESS)
    throw rdr::SystemException("RegEnumKey", result);
  return valueName.buf;
}

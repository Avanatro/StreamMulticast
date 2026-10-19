/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#pragma once

/* Only compiled with -DSMULTI_STREAMLABS_TIKTOK=ON. */

#include <string>

namespace smulti {

/**
 * StreamlabsTokenVault — the imported Streamlabs token, encrypted with
 * Windows DPAPI (current user, constant entropy) in
 * obs_module_config_path("streamlabs_tiktok.bin").
 *
 * The token is never written to config.json, never logged and never shown.
 * Writes are atomic (temp file + rename); plaintext buffers are wiped after
 * use. remove() is the user-facing "Disconnect". Thread safe (no shared state
 * besides the file itself).
 */
namespace streamlabs_vault {

/** Encrypt and store `token`. On failure `error` gets a sanitized reason. */
bool save(const std::string &token, std::string &error);

/** Decrypt the stored token into `token_out`. false when none is stored or it cannot be decrypted. */
bool load(std::string &token_out);

/** Delete the stored token ("Disconnect"). true when no token file remains. */
bool remove();

/** A token file exists (it may still fail to decrypt). */
bool exists();

/** Overwrite a string's bytes with zeros, then clear it. */
void wipe(std::string &secret);

} // namespace streamlabs_vault
} // namespace smulti

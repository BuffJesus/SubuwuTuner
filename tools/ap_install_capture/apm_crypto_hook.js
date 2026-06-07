/*
 * apm_crypto_hook.js — Frida hook for COBB AccessPort Manager.
 *
 * Captures every call to the Windows CryptoAPI (CryptDecrypt, CryptHashData,
 * CryptDeriveKey, CryptSetKeyParam, etc.) and the modern BCrypt API, with
 * full input/output buffer hex dumps.
 *
 * Goal: capture the AES key + IV + cipher mode that APManager uses to
 * decrypt .ptm map files and .img firmware payloads.
 *
 * Usage (headless, no GUI):
 *   frida -f "D:\Programs\Accessport Manager\APManager.exe" \
 *         -l D:\Subuwu\findings\corpus-wide-re-2026-06-06\scripts\apm_crypto_hook.js \
 *         -o D:\Subuwu\findings\apm_crypto_trace.log
 *
 * The -f flag spawns APManager under Frida and immediately attaches.
 * The -o flag tees stdout to the log file.
 *
 * Then drive APManager normally: connect AP, do firmware update, install a map.
 * When done, hit Ctrl-C in the Frida console to detach.
 */

'use strict';

// ---------- buffer dump helpers ----------

function dumpBuffer(ptr, len, maxBytes) {
    if (ptr.isNull() || len === 0) return '(empty)';
    const cap = maxBytes || 4096;
    const n = Math.min(len, cap);
    try {
        const bytes = ptr.readByteArray(n);
        const hex = Array.from(new Uint8Array(bytes)).map(b => b.toString(16).padStart(2, '0')).join('');
        return len > cap ? `${hex}...(${len - cap} more bytes)` : hex;
    } catch (e) {
        return `<read failed: ${e}>`;
    }
}

function readU32(ptr) {
    try { return ptr.readU32(); } catch (e) { return -1; }
}

// ---------- CryptoAPI ALG_ID lookup ----------

const CALG_NAMES = {
    0x6610: 'CALG_AES_256',
    0x660F: 'CALG_AES_192',
    0x660E: 'CALG_AES_128',
    0x6611: 'CALG_AES',
    0x800C: 'CALG_SHA_256',
    0x800D: 'CALG_SHA_384',
    0x800E: 'CALG_SHA_512',
    0x8004: 'CALG_SHA1',
    0x8003: 'CALG_MD5',
    0xA400: 'CALG_RSA_KEYX',
    0x2400: 'CALG_RSA_SIGN',
    0x6603: 'CALG_3DES',
    0x6801: 'CALG_RC4',
    0x6602: 'CALG_RC2',
};

function algName(id) {
    return CALG_NAMES[id] || `0x${id.toString(16)}`;
}

const KP_PARAM_NAMES = {
    0x01: 'KP_IV',
    0x02: 'KP_SALT',
    0x03: 'KP_PADDING',
    0x04: 'KP_MODE',         // 1=CBC 2=ECB 3=OFB 4=CFB 5=CTS (no CTR in legacy CryptoAPI; CTR is typically CNG)
    0x05: 'KP_MODE_BITS',
    0x07: 'KP_PERMISSIONS',
    0x08: 'KP_ALGID',
    0x09: 'KP_BLOCKLEN',
    0x0A: 'KP_KEYLEN',
    0x0B: 'KP_SALT_EX',
    0x0C: 'KP_P',
    0x0D: 'KP_G',
    0x0E: 'KP_Q',
    0x0F: 'KP_X',
    0x10: 'KP_Y',
    0x13: 'KP_PUB_PARAMS',
    0x14: 'KP_VERIFY_PARAMS',
    0x15: 'KP_HIGHEST_VERSION',
    0x16: 'KP_GET_USE_COUNT',
    0x19: 'KP_KEYVAL',
};

const KP_MODE_VALUES = {
    1: 'CRYPT_MODE_CBC',
    2: 'CRYPT_MODE_ECB',
    3: 'CRYPT_MODE_OFB',
    4: 'CRYPT_MODE_CFB',
    5: 'CRYPT_MODE_CTS',
};

// ---------- key & hash handle tracking ----------

const hHashAlgs = {};   // hHash → algorithm name (from CryptCreateHash)
const hKeyMeta  = {};   // hKey  → { algId, mode, iv, derivedFromHash }
const hashSeed  = {};   // hHash → concatenated CryptHashData input (key derivation seed)

let callIdx = 0;
function nextIdx() { return (++callIdx).toString().padStart(6, '0'); }

// ---------- legacy CryptoAPI hooks (advapi32.dll) ----------

function hookCryptoAPI() {
    const advapi32 = Module.findGlobalExportByName('CryptDecrypt');
    if (!advapi32) {
        send({type:'info', msg:'advapi32.CryptDecrypt not resolved — legacy CryptoAPI may not be in use'});
        return;
    }

    // CryptAcquireContextW(phProv, szContainer, szProvider, dwProvType, dwFlags)
    const acq = Module.findGlobalExportByName('CryptAcquireContextW');
    if (acq) Interceptor.attach(acq, {
        onEnter(args) { this.phProv = args[0]; this.provider = args[2]; this.provType = args[3]; },
        onLeave(rv) {
            if (rv.toInt32() === 0) return;
            const id = nextIdx();
            let provStr = '<null>';
            if (!this.provider.isNull()) {
                try { provStr = this.provider.readUtf16String(); } catch (e) {}
            }
            const h = this.phProv.readPointer();
            send({type:'crypt', id, fn:'CryptAcquireContextW', provider: provStr, provType: this.provType.toInt32(), hProv: h.toString()});
        }
    });

    // CryptCreateHash(hProv, AlgId, hKey, dwFlags, phHash)
    const createHash = Module.findGlobalExportByName('CryptCreateHash');
    if (createHash) Interceptor.attach(createHash, {
        onEnter(args) { this.algId = args[1].toInt32(); this.phHash = args[4]; },
        onLeave(rv) {
            if (rv.toInt32() === 0) return;
            const id = nextIdx();
            const h = this.phHash.readPointer().toString();
            hHashAlgs[h] = algName(this.algId);
            hashSeed[h] = '';
            send({type:'crypt', id, fn:'CryptCreateHash', algId: this.algId, algName: algName(this.algId), hHash: h});
        }
    });

    // CryptHashData(hHash, pbData, dwDataLen, dwFlags) — the seed bytes for key derivation
    const hashData = Module.findGlobalExportByName('CryptHashData');
    if (hashData) Interceptor.attach(hashData, {
        onEnter(args) {
            this.hHash = args[0].toString();
            this.pbData = args[1];
            this.dwLen = args[2].toInt32();
        },
        onLeave(rv) {
            if (rv.toInt32() === 0) return;
            const id = nextIdx();
            const hex = dumpBuffer(this.pbData, this.dwLen, 4096);
            hashSeed[this.hHash] = (hashSeed[this.hHash] || '') + hex;
            send({type:'crypt', id, fn:'CryptHashData', hHash: this.hHash, len: this.dwLen, data: hex,
                  algForThisHash: hHashAlgs[this.hHash]});
        }
    });

    // CryptDeriveKey(hProv, Algid, hBaseData (= hash handle), dwFlags, phKey)
    const deriveKey = Module.findGlobalExportByName('CryptDeriveKey');
    if (deriveKey) Interceptor.attach(deriveKey, {
        onEnter(args) {
            this.algId = args[1].toInt32();
            this.hHash = args[2].toString();
            this.dwFlags = args[3].toInt32();
            this.phKey = args[4];
        },
        onLeave(rv) {
            if (rv.toInt32() === 0) return;
            const id = nextIdx();
            const hKey = this.phKey.readPointer().toString();
            hKeyMeta[hKey] = { algId: this.algId, algName: algName(this.algId), hHash: this.hHash, seed: hashSeed[this.hHash] };
            send({type:'crypt', id, fn:'CryptDeriveKey', algId: this.algId, algName: algName(this.algId),
                  hHash: this.hHash, hKey, dwFlags: '0x' + this.dwFlags.toString(16),
                  hashSeed_so_far: hashSeed[this.hHash] || '(none)'});
        }
    });

    // CryptImportKey(hProv, pbData, dwDataLen, hPubKey, dwFlags, phKey)
    const importKey = Module.findGlobalExportByName('CryptImportKey');
    if (importKey) Interceptor.attach(importKey, {
        onEnter(args) {
            this.pbData = args[1];
            this.dwLen = args[2].toInt32();
            this.phKey = args[5];
        },
        onLeave(rv) {
            if (rv.toInt32() === 0) return;
            const id = nextIdx();
            const hKey = this.phKey.readPointer().toString();
            const blob = dumpBuffer(this.pbData, this.dwLen, 4096);
            // Header: BLOBHEADER (8 bytes) followed by key material
            // bType (1) | bVersion (1) | reserved (2) | aiKeyAlg (4)
            let algId = 0;
            try { algId = this.pbData.add(4).readU32(); } catch (e) {}
            hKeyMeta[hKey] = { algId, algName: algName(algId), imported: true };
            send({type:'crypt', id, fn:'CryptImportKey', hKey, algId, algName: algName(algId), blobLen: this.dwLen, blob});
        }
    });

    // CryptSetKeyParam(hKey, dwParam, pbData, dwFlags)
    const setKeyParam = Module.findGlobalExportByName('CryptSetKeyParam');
    if (setKeyParam) Interceptor.attach(setKeyParam, {
        onEnter(args) {
            this.hKey = args[0].toString();
            this.param = args[1].toInt32();
            this.pbData = args[2];
        },
        onLeave(rv) {
            if (rv.toInt32() === 0) return;
            const id = nextIdx();
            const paramName = KP_PARAM_NAMES[this.param] || ('0x' + this.param.toString(16));
            let dataInfo = '';
            // For KP_IV: 16 bytes for AES, 8 for DES
            // For KP_MODE: 4-byte u32 with the mode value
            if (this.param === 0x04) {
                const mode = readU32(this.pbData);
                dataInfo = `mode=${KP_MODE_VALUES[mode] || mode}`;
                if (hKeyMeta[this.hKey]) hKeyMeta[this.hKey].mode = KP_MODE_VALUES[mode] || mode;
            } else if (this.param === 0x01) {
                dataInfo = `iv=${dumpBuffer(this.pbData, 16, 32)}`;
                if (hKeyMeta[this.hKey]) hKeyMeta[this.hKey].iv = dumpBuffer(this.pbData, 16, 16);
            } else if (this.param === 0x19) { // KP_KEYVAL — raw key material (rare)
                dataInfo = `keyval=${dumpBuffer(this.pbData, 32, 32)}`;
            } else {
                dataInfo = `data=${dumpBuffer(this.pbData, 32, 32)}`;
            }
            send({type:'crypt', id, fn:'CryptSetKeyParam', hKey: this.hKey, param: paramName, info: dataInfo,
                  keyMeta: hKeyMeta[this.hKey]});
        }
    });

    // CryptDecrypt(hKey, hHash, Final, dwFlags, pbData, pdwDataLen) — pbData = ciphertext-in / plaintext-out
    const decrypt = Module.findGlobalExportByName('CryptDecrypt');
    if (decrypt) Interceptor.attach(decrypt, {
        onEnter(args) {
            this.hKey = args[0].toString();
            this.final = args[2].toInt32();
            this.pbData = args[4];
            this.pdwDataLen = args[5];
            this.lenIn = readU32(this.pdwDataLen);
            this.ctIn = dumpBuffer(this.pbData, Math.min(this.lenIn, 128), 128);
        },
        onLeave(rv) {
            const id = nextIdx();
            const lenOut = readU32(this.pdwDataLen);
            const ptOut = dumpBuffer(this.pbData, Math.min(lenOut, 128), 128);
            send({type:'crypt', id, fn:'CryptDecrypt', hKey: this.hKey, final: this.final,
                  lenIn: this.lenIn, lenOut, success: rv.toInt32() !== 0,
                  ciphertext_in_first128: this.ctIn,
                  plaintext_out_first128: ptOut,
                  keyMeta: hKeyMeta[this.hKey]});
        }
    });

    const encrypt = Module.findGlobalExportByName('CryptEncrypt');
    if (encrypt) Interceptor.attach(encrypt, {
        onEnter(args) {
            this.hKey = args[0].toString();
            this.pbData = args[4];
            this.pdwDataLen = args[5];
            this.lenIn = readU32(this.pdwDataLen);
            this.ptIn = dumpBuffer(this.pbData, Math.min(this.lenIn, 128), 128);
        },
        onLeave(rv) {
            const id = nextIdx();
            const lenOut = readU32(this.pdwDataLen);
            const ctOut = dumpBuffer(this.pbData, Math.min(lenOut, 128), 128);
            send({type:'crypt', id, fn:'CryptEncrypt', hKey: this.hKey,
                  lenIn: this.lenIn, lenOut, success: rv.toInt32() !== 0,
                  plaintext_in_first128: this.ptIn,
                  ciphertext_out_first128: ctOut,
                  keyMeta: hKeyMeta[this.hKey]});
        }
    });

    send({type:'info', msg:'Legacy CryptoAPI (advapi32) hooks installed'});
}

// ---------- modern BCrypt hooks (bcrypt.dll) ----------

function hookBCrypt() {
    const open = Module.findGlobalExportByName('BCryptOpenAlgorithmProvider');
    if (!open) {
        send({type:'info', msg:'bcrypt.dll not loaded (modern CNG not in use)'});
        return;
    }

    const bcKeyMeta = {};

    Interceptor.attach(open, {
        onEnter(args) {
            this.phAlg = args[0];
            this.pszAlgId = args[1];
            this.pszImpl = args[2];
        },
        onLeave(rv) {
            if (rv.toInt32() !== 0) return;
            const id = nextIdx();
            let algStr = '<null>';
            try { algStr = this.pszAlgId.readUtf16String(); } catch (e) {}
            const h = this.phAlg.readPointer().toString();
            bcKeyMeta[h] = { alg: algStr };
            send({type:'bcrypt', id, fn:'BCryptOpenAlgorithmProvider', alg: algStr, hAlg: h});
        }
    });

    // BCryptSetProperty(hObject, pszProperty, pbInput, cbInput, dwFlags)
    const setProp = Module.findGlobalExportByName('BCryptSetProperty');
    if (setProp) Interceptor.attach(setProp, {
        onEnter(args) {
            this.hObject = args[0].toString();
            this.pszProp = args[1];
            this.pbInput = args[2];
            this.cbInput = args[3].toInt32();
        },
        onLeave(rv) {
            if (rv.toInt32() !== 0) return;
            const id = nextIdx();
            let propStr = '<null>';
            try { propStr = this.pszProp.readUtf16String(); } catch (e) {}
            const buf = dumpBuffer(this.pbInput, this.cbInput, 64);
            let info = '';
            if (propStr === 'ChainingMode') {
                try { info = ` chainingMode=${this.pbInput.readUtf16String()}`; } catch (e) {}
            }
            send({type:'bcrypt', id, fn:'BCryptSetProperty', hObject: this.hObject, prop: propStr,
                  cbInput: this.cbInput, data: buf, info});
        }
    });

    // BCryptGenerateSymmetricKey(hAlgorithm, phKey, pbKeyObject, cbKeyObject, pbSecret, cbSecret, dwFlags)
    const genSymKey = Module.findGlobalExportByName('BCryptGenerateSymmetricKey');
    if (genSymKey) Interceptor.attach(genSymKey, {
        onEnter(args) {
            this.hAlg = args[0].toString();
            this.phKey = args[1];
            this.pbSecret = args[4];
            this.cbSecret = args[5].toInt32();
        },
        onLeave(rv) {
            if (rv.toInt32() !== 0) return;
            const id = nextIdx();
            const hKey = this.phKey.readPointer().toString();
            const secret = dumpBuffer(this.pbSecret, this.cbSecret, 64);
            bcKeyMeta[hKey] = { fromAlg: bcKeyMeta[this.hAlg], secret, cbSecret: this.cbSecret };
            send({type:'bcrypt', id, fn:'BCryptGenerateSymmetricKey',
                  hAlg: this.hAlg, hKey, cbSecret: this.cbSecret,
                  KEY_MATERIAL: secret,
                  alg: bcKeyMeta[this.hAlg]});
        }
    });

    // BCryptDecrypt(hKey, pbInput, cbInput, pPaddingInfo, pbIV, cbIV, pbOutput, cbOutput, pcbResult, dwFlags)
    const bcDecrypt = Module.findGlobalExportByName('BCryptDecrypt');
    if (bcDecrypt) Interceptor.attach(bcDecrypt, {
        onEnter(args) {
            this.hKey = args[0].toString();
            this.pbInput = args[1];
            this.cbInput = args[2].toInt32();
            this.pbIV = args[4];
            this.cbIV = args[5].toInt32();
            this.pbOutput = args[6];
            this.cbOutput = args[7].toInt32();
            this.pcbResult = args[8];
        },
        onLeave(rv) {
            const id = nextIdx();
            const ct = dumpBuffer(this.pbInput, Math.min(this.cbInput, 64), 64);
            const iv = dumpBuffer(this.pbIV, Math.min(this.cbIV, 32), 32);
            let pt = '<no output>';
            try {
                const cbResult = this.pcbResult.readU32();
                pt = dumpBuffer(this.pbOutput, Math.min(cbResult, 64), 64);
            } catch (e) {}
            send({type:'bcrypt', id, fn:'BCryptDecrypt', hKey: this.hKey,
                  cbInput: this.cbInput, cbIV: this.cbIV, cbOutput: this.cbOutput,
                  IV: iv, ciphertext_first64: ct, plaintext_first64: pt,
                  status: rv.toInt32(),
                  keyMeta: bcKeyMeta[this.hKey]});
        }
    });

    const bcEncrypt = Module.findGlobalExportByName('BCryptEncrypt');
    if (bcEncrypt) Interceptor.attach(bcEncrypt, {
        onEnter(args) {
            this.hKey = args[0].toString();
            this.pbInput = args[1];
            this.cbInput = args[2].toInt32();
            this.pbIV = args[4];
            this.cbIV = args[5].toInt32();
            this.pbOutput = args[6];
            this.cbOutput = args[7].toInt32();
            this.pcbResult = args[8];
        },
        onLeave(rv) {
            const id = nextIdx();
            const pt = dumpBuffer(this.pbInput, Math.min(this.cbInput, 64), 64);
            const iv = dumpBuffer(this.pbIV, Math.min(this.cbIV, 32), 32);
            let ct = '<no output>';
            try {
                const cbResult = this.pcbResult.readU32();
                ct = dumpBuffer(this.pbOutput, Math.min(cbResult, 64), 64);
            } catch (e) {}
            send({type:'bcrypt', id, fn:'BCryptEncrypt', hKey: this.hKey,
                  cbInput: this.cbInput, cbIV: this.cbIV,
                  IV: iv, plaintext_first64: pt, ciphertext_first64: ct,
                  status: rv.toInt32(),
                  keyMeta: bcKeyMeta[this.hKey]});
        }
    });

    send({type:'info', msg:'BCrypt (CNG) hooks installed'});
}

// ---------- file I/O hooks for context (so we know what file's being processed) ----------

// Track interesting file handles → file paths so ReadFile knows what file it's reading
const handleToPath = {};

function hookFileIO() {
    // CreateFileW(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile)
    const createFile = Module.findGlobalExportByName('CreateFileW');
    if (createFile) Interceptor.attach(createFile, {
        onEnter(args) { this.fname = args[0]; },
        onLeave(rv) {
            if (rv.toInt32() === -1) return;
            try {
                const s = this.fname.readUtf16String();
                if (s && (s.endsWith('.ptm') || s.endsWith('.img') || s.endsWith('.rom') || s.includes('mapdatacache'))) {
                    const hFile = rv.toString();
                    handleToPath[hFile] = s;
                    send({type:'file', id: nextIdx(), fn: 'CreateFileW', path: s, hFile});
                }
            } catch (e) {}
        }
    });

    // CloseHandle — clean up tracking
    const closeHandle = Module.findGlobalExportByName('CloseHandle');
    if (closeHandle) Interceptor.attach(closeHandle, {
        onEnter(args) {
            const h = args[0].toString();
            if (handleToPath[h]) {
                delete handleToPath[h];
            }
        }
    });

    // ReadFile(hFile, lpBuffer, nNumberOfBytesToRead, lpNumberOfBytesRead, lpOverlapped)
    // ONLY hook reads on .ptm/.img/.rom files (filtered via handleToPath)
    const readFile = Module.findGlobalExportByName('ReadFile');
    if (readFile) Interceptor.attach(readFile, {
        onEnter(args) {
            this.hFile = args[0].toString();
            this.buf = args[1];
            this.nReq = args[2].toInt32();
            this.pnRead = args[3];
            this.path = handleToPath[this.hFile];
        },
        onLeave(rv) {
            if (!this.path) return;            // not a tracked file
            if (rv.toInt32() === 0) return;    // ReadFile returned FALSE
            let nRead = this.nReq;
            try { if (!this.pnRead.isNull()) nRead = this.pnRead.readU32(); } catch (e) {}
            if (nRead === 0) return;
            const dump = dumpBuffer(this.buf, Math.min(nRead, 128), 128);
            send({type:'file', id: nextIdx(), fn: 'ReadFile',
                  path: this.path, hFile: this.hFile,
                  nRequested: this.nReq, nRead, bufAddr: this.buf.toString(),
                  first128: dump});
        }
    });

    // WriteFile too — capture decrypted output if it goes to disk
    const writeFile = Module.findGlobalExportByName('WriteFile');
    if (writeFile) Interceptor.attach(writeFile, {
        onEnter(args) {
            this.hFile = args[0].toString();
            this.buf = args[1];
            this.nReq = args[2].toInt32();
            this.path = handleToPath[this.hFile];
        },
        onLeave(rv) {
            if (!this.path) return;
            if (rv.toInt32() === 0) return;
            if (this.nReq === 0) return;
            const dump = dumpBuffer(this.buf, Math.min(this.nReq, 128), 128);
            send({type:'file', id: nextIdx(), fn: 'WriteFile',
                  path: this.path, hFile: this.hFile,
                  nWritten: this.nReq, bufAddr: this.buf.toString(),
                  first128: dump});
        }
    });

    send({type:'info', msg:'File I/O hooks installed (CreateFileW/ReadFile/WriteFile filtered to .ptm/.img/.rom/mapdatacache)'});
}

// ---------- static AES function discovery via memory-access watchpoint ----------

// Static analysis of APManager.exe showed AES tables at these FILE offsets:
//   - AES S-box (256 bytes): file 0x6FFA68
//   - AES Inv-Sbox (256 B):  file 0x6FFB68
//   - AES T0 (1024 B):       file 0x72A298
//   - AES Rcon:              file 0x72D298
//
// The PE section .rdata is at file off 0x4F5400 → virt offset 0x4F6000, so
// the virt offset = file_off - 0x4F5400 + 0x4F6000 = file_off + 0xC00.
// Absolute address = APManager.exe base + virt_offset.
//
// We set MemoryAccessMonitor on the S-box range; the first read tells us
// the AES function PC.

const AES_TABLE_RVA = {
    sbox:     0x6FFA68 - 0x4F5400 + 0x4F6000,   // = 0x700668
    invSbox:  0x6FFB68 - 0x4F5400 + 0x4F6000,   // = 0x700768
    t0:       0x72A298 - 0x4F5400 + 0x4F6000,
    rcon:     0x72D298 - 0x4F5400 + 0x4F6000,
};

const aesCallSites = new Set();  // PC values that have hit AES tables
let aesWatchSetup = false;

function setupAesWatch() {
    if (aesWatchSetup) return;
    const apm = Process.findModuleByName('APManager.exe') || Process.getModuleByName('APManager.exe');
    if (!apm) {
        send({type:'info', msg:'APManager.exe module not yet loaded — deferring AES watch'});
        return;
    }
    const base = apm.base;
    const sboxAddr = base.add(AES_TABLE_RVA.sbox);
    const invSboxAddr = base.add(AES_TABLE_RVA.invSbox);
    const t0Addr = base.add(AES_TABLE_RVA.t0);
    const rconAddr = base.add(AES_TABLE_RVA.rcon);

    // Precise table ranges for filtering (page-granularity hooks fire on neighboring data too)
    const tableRanges = [
        {name: 'sbox',    base: sboxAddr,    size: 256},
        {name: 'invSbox', base: invSboxAddr, size: 256},
        {name: 't0',      base: t0Addr,      size: 1024},
        {name: 'rcon',    base: rconAddr,    size: 32},
    ];

    function classify(addr) {
        for (const r of tableRanges) {
            const off = addr.sub(r.base).toInt32();
            if (off >= 0 && off < r.size) return {table: r.name, off};
        }
        return null;
    }

    // Watch S-box page AND T0 page (both 4 KB).  Inv-Sbox sits in same page as S-box.
    const watchRanges = [
        {base: sboxAddr, size: 256},   // page-aligned, covers S-box + Inv-Sbox
        {base: t0Addr,   size: 1024},  // T0 table (4 lookup tables for fast AES)
    ];

    send({type:'info', msg:`APManager base=${base}, AES S-box virt=${sboxAddr}, T0 virt=${t0Addr}`});

    let totalAccess = 0;
    let realAesHits = 0;
    try {
        MemoryAccessMonitor.enable(watchRanges, {
            onAccess(details) {
                totalAccess++;
                const classification = classify(details.address);
                if (!classification) return;   // page hit but not actual AES table — drop
                realAesHits++;
                const pc = details.from;
                const pcStr = pc.toString();
                const fnOff = pc.sub(base).toInt32();
                const key = `${pcStr}|${classification.table}`;
                if (aesCallSites.has(key)) return;
                aesCallSites.add(key);
                send({type:'aes_access', id: nextIdx(),
                      pc: pcStr,
                      table: classification.table,
                      table_offset: classification.off,
                      operation: details.operation,
                      address: details.address.toString(),
                      from_offset_in_apm: '0x' + fnOff.toString(16),
                      total_accesses_so_far: totalAccess,
                      real_aes_hits_so_far: realAesHits});
            }
        });
        aesWatchSetup = true;
        send({type:'info', msg:`MemoryAccessMonitor armed on AES tables (page-granularity, range-filtered for precision)`});
    } catch (e) {
        send({type:'error', msg:`AES watch setup failed: ${e}`});
    }
}

// ---------- install all hooks ----------

try {
    hookCryptoAPI();
    hookBCrypt();
    hookFileIO();
    setupAesWatch();
    send({type:'info', msg:'All hooks installed; ready to capture.'});
} catch (e) {
    send({type:'error', msg: 'Hook setup failed: ' + e.toString() + '\n' + e.stack});
}

"""Byte-for-byte Device Auth v2 signing contract matching the shared Python reference."""


def signing_bytes(device_id, product_key, key_version, firmware_version, device_nonce,
                  timestamp, challenge, purpose, transport):
    return '\n'.join((
        'YGSoul-Device-Auth-v2', device_id, product_key,
        str(key_version), firmware_version, device_nonce,
        str(timestamp), challenge, purpose, transport, '2',
    )).encode('utf-8')

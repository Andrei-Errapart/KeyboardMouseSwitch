"""The constants the firmware self-test (kmb selftest) checks, recomputed
with an independent AES-CCM implementation. The firmware copies these
values into src/link.c; a typo there fails the self-test, a wrong value
here fails this test."""
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.ciphers.aead import AESCCM

PSK = b"KMS bench key 01"
SALT_B = bytes(range(0x10, 0x18))
SALT_R = bytes(range(0x20, 0x28))
SK = bytes.fromhex("b483529cc31c16b598df0b30135f3083")
LAYOUT_NONCE = bytes.fromhex("05000000" "01" "0000000000000000")
LAYOUT_PLAIN = bytes(range(0x30, 0x41))
LAYOUT_SEALED = bytes.fromhex("5564a62093c965a7aa7f9aac0fde39e6bf0ff053fb")


def test_nist_sp800_38c_example_1():
    ccm = AESCCM(bytes(range(0x40, 0x50)), tag_length=4)
    assert ccm.encrypt(bytes(range(0x10, 0x17)), bytes([0x20, 0x21, 0x22, 0x23]),
                       bytes(range(8))).hex() == "7162015b4dac255d"


def test_nist_sp800_38c_example_2():
    # Full blocks of associated data and payload, and a 6-byte tag: the
    # firmware's CCM must not depend on the 4-byte MIC or partial blocks.
    ccm = AESCCM(bytes(range(0x40, 0x50)), tag_length=6)
    assert ccm.encrypt(bytes(range(0x10, 0x18)), bytes(range(0x20, 0x30)),
                       bytes(range(0x10))).hex() == (
        "d2a1f0e051ea5f62081a7792073d593d1fc64fbfaccd")


def test_session_key_is_aes_ecb_of_both_salts():
    enc = Cipher(algorithms.AES(PSK), modes.ECB()).encryptor()
    assert enc.update(SALT_B + SALT_R) + enc.finalize() == SK


def test_session_packet_layout_vector():
    # Nonce: counter 5 little-endian, direction 1 (receiver to bridge),
    # eight zero bytes. 17-byte body, 4-byte MIC, no associated data.
    assert len(LAYOUT_PLAIN) == 17
    assert AESCCM(SK, tag_length=4).encrypt(LAYOUT_NONCE, LAYOUT_PLAIN,
                                            None) == LAYOUT_SEALED

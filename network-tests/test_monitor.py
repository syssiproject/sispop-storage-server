from util import sn_address
import ss
import subkey
import time
import datetime
from nacl.hash import blake2b
from nacl.encoding import RawEncoder, Base64Encoder
from nacl.signing import SigningKey, VerifyKey
import nacl.bindings as sodium
import json
import base64

import sispopmq
from sispopc import bt_serialize, bt_deserialize


def notify_request(
    sk: SigningKey,
    ts: int,
    data: bool,
    namespaces: list,
    *,
    netid=0x05,
    sessionid: bool = False,
    subk: bytes = None,
):

    req = {'n': sorted(namespaces), 'd': int(data), 't': ts}

    if sessionid:
        assert netid == 0x05
        req['P'] = sk.verify_key.encode()
        account = b'\x05' + sk.verify_key.to_curve25519_public_key().encode()
    else:
        account = chr(netid).encode() + sk.verify_key.encode()
        req['p'] = account

    # ( "MONITOR" || ACCOUNT || TS || D || NS[0] || ... || NS[n] )
    message = (
        f'MONITOR{account.hex()}{ts:d}{data:d}' + ','.join(f'{n}' for n in req['n'])
    ).encode()

    if not subk:
        req['s'] = sk.sign(message).signature
    else:
        assert len(subk) == 32
        req['S'] = subk
        _, d, D = subkey.make_subkey(sk, subk)
        sig = subkey.sign(message, sk, d, D)
        assert len(sig) == 64
        req['s'] = sig

    return req


def test_monitor_reg_ed(omq, random_sn, sk, exclude):
    swarm = ss.get_swarm(omq, random_sn, sk)

    o = sispopmq.SispopMQ()
    o.start()
    ts = int(time.time())
    registered = []
    for snode in swarm['snodes']:
        snode['addr'] = sispopmq.Address(
            f"curve://{snode['ip']}:{snode['port_omq']}/{snode['pubkey_x25519']}"
        )
        c = o.connect_remote(
            snode['addr'],
            on_success=lambda conn: None,
            on_failure=lambda _, msg: print(f"Connection failed: {msg}"),
            timeout=datetime.timedelta(seconds=3),
        )
        req = notify_request(sk, ts, True, [-5, 0, 23], netid=3)
        registered.append(
            o.request_future(
                c,
                "monitor.messages",
                bt_serialize(req),
                request_timeout=datetime.timedelta(seconds=7),
            )
        )

    registered = [r.get() for r in registered]
    assert registered == [[b'd7:successi1ee']] * len(registered)


def test_monitor_reg_session(omq, random_sn, sk, exclude):
    swarm = ss.get_swarm(omq, random_sn, sk)

    o = sispopmq.SispopMQ()
    o.start()
    ts = int(time.time())
    registered = []
    for snode in swarm['snodes']:
        snode['addr'] = sispopmq.Address(
            f"curve://{snode['ip']}:{snode['port_omq']}/{snode['pubkey_x25519']}"
        )
        c = o.connect_remote(
            snode['addr'],
            on_success=lambda conn: None,
            on_failure=lambda _, msg: print(f"Connection failed: {msg}"),
            timeout=datetime.timedelta(seconds=3),
        )
        req = notify_request(sk, ts, True, [-5, 0, 23], netid=5, sessionid=True)
        registered.append(
            o.request_future(
                c,
                "monitor.messages",
                bt_serialize(req),
                request_timeout=datetime.timedelta(seconds=7),
            )
        )

    registered = [r.get() for r in registered]
    assert registered == [[b'd7:successi1ee']] * len(registered)


def test_monitor_reg_subkey(omq, random_sn, sk, exclude):
    swarm = ss.get_swarm(omq, random_sn, sk)

    # Highly random subkey tag:
    subk = b'abcdefghijklmnopqrstuvwxyzomg123'
    o = sispopmq.SispopMQ()
    o.start()
    ts = int(time.time())
    registered = []
    for snode in swarm['snodes']:
        snode['addr'] = sispopmq.Address(
            f"curve://{snode['ip']}:{snode['port_omq']}/{snode['pubkey_x25519']}"
        )
        c = o.connect_remote(
            snode['addr'],
            on_success=lambda conn: None,
            on_failure=lambda _, msg: print(f"Connection failed: {msg}"),
            timeout=datetime.timedelta(seconds=3),
        )
        req = notify_request(sk, ts, True, [-5, 0, 23], netid=2, subk=subk)
        registered.append(
            o.request_future(
                c,
                "monitor.messages",
                bt_serialize(req),
                request_timeout=datetime.timedelta(seconds=7),
            )
        )

    registered = [r.get() for r in registered]
    assert registered == [[b'd7:successi1ee']] * len(registered)


def test_monitor_push(omq, random_sn, sk, exclude):
    swarm = ss.get_swarm(omq, random_sn, sk)

    conns = {}

    n_notifies = 0

    def handle_notify_message(m):
        nonlocal conns, n_notifies
        snode = conns[m.conn]
        print(f"got notify from {snode['pubkey_legacy']} at {time.time()}")
        conns[m.conn]['response'].append(bt_deserialize(m.data()[0]))
        n_notifies += 1

    # We need to make our own OMQ because we need to add the cat/command for notifies
    o = sispopmq.SispopMQ()
    o.max_message_size = 10 * 1024 * 1024
    notify = o.add_category('notify', sispopmq.AuthLevel.none)
    notify.add_command("message", handle_notify_message)
    o.start()

    ts = int(time.time())
    registered = []
    for snode in swarm['snodes']:
        snode['response'] = []
        c = o.connect_remote(
            sispopmq.Address(f"curve://{snode['ip']}:{snode['port_omq']}/{snode['pubkey_x25519']}"),
            on_success=lambda conn: connected.add(conn),
            on_failure=lambda _, msg: print(f"Connection failed: {msg}"),
        )
        snode['conn'] = c
        conns[c] = snode

        registered.append(
            o.request_future(
                c,
                "monitor.messages",
                bt_serialize(notify_request(sk, ts, True, [-5, 0, 23], netid=3)),
                request_timeout=datetime.timedelta(seconds=5),
            )
        )

    registered = [r.get() for r in registered]
    assert registered == [[b'd7:successi1ee']] * len(registered)

    # Now go send a message:
    sn = ss.random_swarm_members(swarm, 1, exclude)[0]
    conn = omq.connect_remote(sn_address(sn))

    print(f"starting store at {time.time()}")
    ts = int(time.time() * 1000)
    ttl = 86400000
    exp = ts + ttl
    # Store a message for myself
    s = omq.request_future(
        conn,
        'storage.store',
        [
            json.dumps(
                {
                    "pubkey": '03' + sk.verify_key.encode().hex(),
                    "timestamp": ts,
                    "ttl": ttl,
                    "data": base64.b64encode("abc 123".encode()).decode(),
                }
            ).encode()
        ],
    )

    # And another, but this one in a non-monitored namespace:
    s2 = omq.request_future(
        conn,
        'storage.store',
        [
            json.dumps(
                {
                    "pubkey": '03' + sk.verify_key.encode().hex(),
                    "timestamp": ts,
                    "namespace": 123,
                    "ttl": ttl,
                    "data": base64.b64encode("abc 123".encode()).decode(),
                }
            ).encode()
        ],
    )

    # It's pretty rare that we don't get all the responses before the store responses (since they
    # don't have to be onion-routed back to us), but give it a couple seconds anyway.
    s = s.get()
    print(f"got store response at {time.time()}")
    assert len(s) == 1
    s = json.loads(s[0])
    hash = (
        blake2b(
            b'\x03' + sk.verify_key.encode() + b'abc 123',
            encoder=Base64Encoder,
        )
        .decode()
        .rstrip('=')
    )
    assert [v['hash'] for v in s['swarm'].values()] == [hash] * len(s['swarm'])

    s2 = s2.get()

    tries = 0
    while n_notifies < len(swarm['snodes']) and tries < 8:
        time.sleep(0.25)
        tries += 1

    expected_notify = {
        b'@': b'\x03' + sk.verify_key.encode(),
        b'h': hash.encode(),
        b'n': 0,
        b't': ts,
        b'z': exp,
        b'~': b'abc 123',
    }

    assert [s['response'] for s in swarm['snodes']] == [[expected_notify]] * len(swarm['snodes'])


def test_monitor_multi(omq, random_sn, sk, exclude):
    swarm = ss.get_swarm(omq, random_sn, sk)

    conns = {}

    n_notifies = 0

    sk2 = SigningKey.generate()

    def handle_notify_message(m):
        nonlocal conns, n_notifies
        snode = conns[m.conn]
        print(f"got notify from {snode['pubkey_legacy']} at {time.time()}")
        conns[m.conn]['response'].append(bt_deserialize(m.data()[0]))
        n_notifies += 1

    # We need to make our own OMQ because we need to add the cat/command for notifies
    o = sispopmq.SispopMQ()
    o.max_message_size = 10 * 1024 * 1024
    notify = o.add_category('notify', sispopmq.AuthLevel.none)
    notify.add_command("message", handle_notify_message)
    o.start()

    ts = int(time.time())
    registered = []
    for snode in swarm['snodes']:
        snode['response'] = []
        c = o.connect_remote(
            sispopmq.Address(f"curve://{snode['ip']}:{snode['port_omq']}/{snode['pubkey_x25519']}"),
            on_success=lambda conn: connected.add(conn),
            on_failure=lambda _, msg: print(f"Connection failed: {msg}"),
        )
        snode['conn'] = c
        conns[c] = snode

        registered.append(
            o.request_future(
                c,
                "monitor.messages",
                bt_serialize(
                    [
                        notify_request(sk2, ts, True, [0], netid=3),
                        notify_request(sk, ts, True, [-5, 0, 23], netid=3),
                    ]
                ),
                request_timeout=datetime.timedelta(seconds=5),
            )
        )

    registered = [r.get() for r in registered]
    assert registered == [[b'l' + b'd7:successi1ee' * 2 + b'e']] * len(registered)

    # Now go send a message:
    sn = ss.random_swarm_members(swarm, 1, exclude)[0]
    conn = omq.connect_remote(sn_address(sn))

    print(f"starting store at {time.time()}")
    ts = int(time.time() * 1000)
    ttl = 86400000
    exp = ts + ttl
    # Store a message for myself
    s = omq.request_future(
        conn,
        'storage.store',
        [
            json.dumps(
                {
                    "pubkey": '03' + sk.verify_key.encode().hex(),
                    "timestamp": ts,
                    "ttl": ttl,
                    "data": base64.b64encode("xyz 123".encode()).decode(),
                }
            ).encode()
        ],
    )

    s = s.get()
    print(f"got store response at {time.time()}")
    assert len(s) == 1
    s = json.loads(s[0])

    tries = 0
    while n_notifies < len(swarm['snodes']) and tries < 8:
        time.sleep(0.25)
        tries += 1

    for sn in s['swarm'].values():
        hash = sn['hash']
        break

    expected_notify = {
        b'@': b'\x03' + sk.verify_key.encode(),
        b'h': hash.encode(),
        b'n': 0,
        b't': ts,
        b'z': exp,
        b'~': b'xyz 123',
    }

    assert [s['response'] for s in swarm['snodes']] == [[expected_notify]] * len(swarm['snodes'])

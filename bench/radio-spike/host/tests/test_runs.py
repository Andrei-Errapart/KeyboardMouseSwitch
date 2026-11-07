from kmbench import runs


class FakeRecorder:
    def __init__(self):
        self.sent = []

    def send(self, line):
        self.sent.append(line)

    def mark(self):
        return len(self.sent)


def test_reset_restores_default_settings():
    # `kmb set` values stay on the dongle across roles: a run that sets
    # retries_report 0 (T7 under load) would otherwise leave every later
    # run sending reports without retries.
    b, r = FakeRecorder(), FakeRecorder()
    runs.reset(b, r, settle=0)
    for x in (b, r):
        assert x.sent == ["kmb role none", "kmb set defaults"]

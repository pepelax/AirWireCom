class HexRecordError(Exception):
    pass

class IntelHex(dict):
    def __init__(self, *args, **kwargs):
        super().__init__()
        self.padding = 0xFF
    def loadfile(self, *args, **kwargs):
        raise HexRecordError("intelhex module not available (stub)")
    def tobinfile(self, *args, **kwargs):
        raise HexRecordError("intelhex module not available (stub)")
    def write_hex_file(self, *args, **kwargs):
        raise HexRecordError("intelhex module not available (stub)")

require 'serialport'

module RemoteKeyboard
  class Interface
    def initialize(_port, _baud)
      @port = SerialPort.new(_port, _baud, 8, 1, SerialPort::NONE)
    end

    def close
      @port.close unless @port.closed?
    end

    def read_data_available?(timeout = 0)
      (rh, wh, eh) = IO::select([@sp], nil, nil, timeout)
      ! rh.nil?
    end

    def read_all_available_bytes(timeout =0, max = 1000)
      if read_data_available?(timeout)
        r = @sp.sysread(max)
        @last_receive = Time.now
        r
      else
        ''
      end
    end

    def write(data)
      @sp.write(data)
    end
  end

  class Key
    @keys = {}
    class << self
      def addKey(key)
        @keys[key.str] = key
      end

      def key[str]
        @keys[str]
      end

      def allKeys
        @keys
      end
    end

    def initialize(_code, _str)
      @code = '%02d' % _code.to_i 
      @str = _str
      @pressed = false
      Key.addKey(self)
    end

    attr_reader str

    def pressed=(state)
      @pressed = state
    end

    def pressed
      @pressed
    end

    # returns string or nil if already pressed
    def press
      unless pressed
        pressed= true
        return "p" + @code + "\r"
      end
    end

    # returns string or nil if already released
    def release
      if pressed
        pressed= false
        return "r" + @code + "\r"
      end
    end

    def modify(s)
      s
    end
  end

  class ShiftedKey < Key
  end

  class ShiftKey < Key
  end

  class TogglingShiftKey < Key
  end

end

class BrotherPTouchHomeAndHobby < RemoteKeyboard
  # row/column	key   code-shift
  # Printing characters
  KEY_DEFS = [[ 72, ' ', 'Feed' ],
  [ 71, ',', ':' ],
  [ 42, '.', '?' ],
  [ 53, "'", '/' ],
  [ 02, '"', '!' ],
  [ 54, '&', 'Size' ],
  [ 64, 'A', 'Style' ],
  [ 13, 'B', '¢' ],
  [ 73, 'C', '@' ],
  [ 74, 'D', 'Frame' ],
  [ 35, 'E', '3' ],
  [ 24, 'F', 'Accent' ],
  [ 14, 'G', 'Repeat' ],
  [ 44, 'H', 'Check' ],
  [ 05, 'I', '8' ],
  [ 04, 'J', 'Preset' ],
  [ 01, 'K', '(' ],
  [ 41, 'L', ')' ],
  [ 03, 'M', '.' ],
  [ 43, 'N', '<telephone>' ],
  [ 11, 'O', '9' ],
  [ 21, 'P', '0' ],
  [ 55, 'Q', '1' ],
  [ 75, 'R', '4' ],
  [ 34, 'S', 'Underline' ],
  [ 25, 'T', '5' ],
  [ 45, 'U', '7' ],
  [ 23, 'V', '$' ],
  [ 65, 'W', '2' ],
  [ 33, 'X', '~' ],
  [ 15, 'Y', '6' ],
  [ 63, 'Z', '-' ],
  [ 22, 'Return' ],
  # Control keys
  [ 31, 'BS', 'Clear' ],
  [ 51, 'LArrow', 'Home' ],
  [ 06, 'Power' ],
  [ 50, 'Print' ],
  [ 61, 'RArrow', 'End' ],
  [ 60, 'Sym' ],
  # Modifier keys
  [ 52, 'LCode' ],
  [ 12, 'RCode' ],
  # sticky mod keys
  [ 62, 'Caps' ],
  [ 32, 'Num' ]]
end

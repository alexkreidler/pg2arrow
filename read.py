from json import load
import pyarrow as pa

with pa.memory_map('limited.arrow', 'r') as source:
    loaded_arrays = pa.ipc.open_file(source).read_all()
    print(loaded_arrays[0].schema)
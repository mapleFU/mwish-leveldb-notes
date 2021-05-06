```rust
use std::path::Path;

use leveldb::database::Database;
use leveldb::iterator::Iterable;
use leveldb::kv::KV;
use leveldb::options::{Options, WriteOptions, ReadOptions};

fn main() {
    let path = Path::new("nmsl");
    let mut options = Options::new();
    options.create_if_missing = true;
    let mut database = match Database::open(path, options) {
        Ok(db) => { db },
        Err(e) => { panic!("failed to open database: {:?}", e) }
    };
  
    let write_opts = WriteOptions::new();
    match database.put(write_opts, 1, &[1]) {
        Ok(_) => { () },
        Err(e) => { panic!("failed to write to database: {:?}", e) }
    };
  
    let read_opts = ReadOptions::new();
    let res = database.get(read_opts, 1);
  
    match res {
      Ok(data) => {
        assert!(data.is_some());
        assert_eq!(data, Some(vec![1]));
      }
      Err(e) => { panic!("failed reading data: {:?}", e) }
    }
  
    let read_opts = ReadOptions::new();
    let mut iter = database.iter(read_opts);
    let entry = iter.next();
    assert_eq!(
      entry,
      Some((1, vec![1]))
    );

    for _ in 0..10000 {
        database.put(write_opts, 1, &[1, 2, 3, 4, 5, 6, 7, 8, 9, 100]).unwrap();
        database.delete(write_opts, 1).unwrap();
    }
}
```

使用上面的脚本，创建 `ldb` 文件，然后使用 `./leveldbutil dump ` 来读，具体内容如下:

```c++
'\x00\x00\x00\x01' @ 19 : del => ''
'\x00\x00\x00\x01' @ 18 : val => '\x01\x02\x03\x04\x05\x06\x07\x08\x09d'
'\x00\x00\x00\x01' @ 17 : del => ''
'\x00\x00\x00\x01' @ 16 : val => '\x01\x02\x03\x04\x05\x06\x07\x08\x09d'
'\x00\x00\x00\x01' @ 15 : del => ''
'\x00\x00\x00\x01' @ 14 : val => '\x01\x02\x03\x04\x05\x06\x07\x08\x09d'
'\x00\x00\x00\x01' @ 13 : del => ''
'\x00\x00\x00\x01' @ 12 : val => '\x01\x02\x03\x04\x05\x06\x07\x08\x09d'
'\x00\x00\x00\x01' @ 11 : del => ''
'\x00\x00\x00\x01' @ 10 : val => '\x01\x02\x03\x04\x05\x06\x07\x08\x09d'
'\x00\x00\x00\x01' @ 9 : del => ''
'\x00\x00\x00\x01' @ 8 : val => '\x01\x02\x03\x04\x05\x06\x07\x08\x09d'
'\x00\x00\x00\x01' @ 7 : del => ''
'\x00\x00\x00\x01' @ 6 : val => '\x01\x02\x03\x04\x05\x06\x07\x08\x09d'
'\x00\x00\x00\x01' @ 5 : del => ''
'\x00\x00\x00\x01' @ 4 : val => '\x01\x02\x03\x04\x05\x06\x07\x08\x09d'
'\x00\x00\x00\x01' @ 3 : del => ''
'\x00\x00\x00\x01' @ 2 : val => '\x01\x02\x03\x04\x05\x06\x07\x08\x09d'
'\x00\x00\x00\x01' @ 1 : val => '\x01'
```

可以发现，如代码所示\(老实说，我看了代码还觉得自己哪里搞错了，然后试了下\)，L0 文件会保存所有的记录。

C++ 使用 LevelDB 大概可以写 CMake:

```c++
# - Find LevelDB
#
#  LevelDB_INCLUDES  - List of LevelDB includes
#  LevelDB_LIBRARIES - List of libraries when using LevelDB.
#  LevelDB_FOUND     - True if LevelDB found.

# Look for the header file.
find_path(LevelDB_INCLUDE NAMES leveldb/db.h
        PATHS $ENV{LEVELDB_ROOT}/include /opt/local/include /usr/local/include /usr/include
        DOC "Path in which the file leveldb/db.h is located." )

# Look for the library.
find_library(LevelDB_LIBRARY NAMES leveldb
        PATHS /usr/lib $ENV{LEVELDB_ROOT}/lib
        DOC "Path to leveldb library." )

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(LevelDB DEFAULT_MSG LevelDB_INCLUDE LevelDB_LIBRARY)

if(LEVELDB_FOUND)
    message(STATUS "Found LevelDB (include: ${LevelDB_INCLUDE}, library: ${LevelDB_LIBRARY})")
    set(LevelDB_INCLUDES ${LevelDB_INCLUDE})
    set(LevelDB_LIBRARIES ${LevelDB_LIBRARY})
    mark_as_advanced(LevelDB_INCLUDE LevelDB_LIBRARY)

    if(EXISTS "${LevelDB_INCLUDE}/leveldb/db.h")
        file(STRINGS "${LevelDB_INCLUDE}/leveldb/db.h" __version_lines
                REGEX "static const int k[^V]+Version[ \t]+=[ \t]+[0-9]+;")

        foreach(__line ${__version_lines})
            if(__line MATCHES "[^k]+kMajorVersion[ \t]+=[ \t]+([0-9]+);")
                set(LEVELDB_VERSION_MAJOR ${CMAKE_MATCH_1})
            elseif(__line MATCHES "[^k]+kMinorVersion[ \t]+=[ \t]+([0-9]+);")
                set(LEVELDB_VERSION_MINOR ${CMAKE_MATCH_1})
            endif()
        endforeach()

        if(LEVELDB_VERSION_MAJOR AND LEVELDB_VERSION_MINOR)
            set(LEVELDB_VERSION "${LEVELDB_VERSION_MAJOR}.${LEVELDB_VERSION_MINOR}")
        endif()
    endif()
endif()


include_directories(
        "${PROJECT_BINARY_DIR}/include"
        "${PROJECT_SOURCE_DIR}"
        "${LevelDB_INCLUDES}"
)
```


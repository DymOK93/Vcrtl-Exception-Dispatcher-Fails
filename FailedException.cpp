
#include <ntddk.h>

struct Exception {
  Exception() { DbgPrint("Exception object was created\n"); }
  ~Exception() { DbgPrint("Exception object was destroyed\n"); }
};

template <class Fn, class... Types>
__declspec(noinline) decltype(auto)
    MakeCall(PUNICODE_STRING name, Types... args) {
  const auto fn{static_cast<Fn>(MmGetSystemRoutineAddress(name))};
  char buffer[256];
  if (fn) {
    memcpy(buffer, fn(args...),
           name->Length);  // Force insertion a __GSHandlerCheck() info
                           // exception handler table
  } else {
    throw Exception{};
  }
}

template <unsigned N>
struct ThrowAtNth {
  static inline unsigned instance_count{0};
  static constexpr unsigned max_count{N};

  ThrowAtNth() {
    DbgPrint("Created %u instance of ThrowAtNth<%u>\n", ++instance_count, N);
    if (instance_count == max_count) {
      throw Exception{};
    }
  }

  ThrowAtNth(int) : ThrowAtNth() {}

  ThrowAtNth(const ThrowAtNth&) = delete;
  ThrowAtNth& operator=(const ThrowAtNth&) = delete;

  ~ThrowAtNth() {
    DbgPrint("Destoyed %u instance ofThrowAtNth<%u>\n", instance_count--, N);
  }
};

template <unsigned N>
struct ThrowInDtor {
  static inline unsigned instance_count{0};
  static constexpr unsigned min_count{N};

  ThrowInDtor() {
    DbgPrint("Created %u instance of ThrowInDtor<%u>\n", ++instance_count, N);
  }

  ThrowInDtor(int) : ThrowAtNth() {}

  ThrowInDtor(const ThrowInDtor&) = delete;
  ThrowInDtor& operator=(const ThrowInDtor&) = delete;

  ~ThrowInDtor() {
    DbgPrint("Destoyed %u instance of ThrowInDtor<%u>\n", instance_count--, N);
    if (instance_count == min_count) {
#pragma warning(suppress : 4297)  // It's not a production code :)
      throw Exception{};
    }
  }
};

template <class Ty, size_t N>
struct Array {
  template <class... Types>
  Array(Types... args) : data{args...} {}

  Ty data[N];
};

#define PRINT_CATCH(obj, N)                           \
  DbgPrint("%s: exception caught!\n", __FUNCTION__);  \
  DbgPrint("Leaked %u instances of ThrowAtNth<%u>\n", \
           obj##<N>::instance_count, ThrowAtNth<N>::max_count);

void NotAllObjectsAreDestroyed() noexcept;
void AggregateProblem() noexcept;
void ExcInDtor() noexcept;
void NoexceptViolated() noexcept;

void BufferSecurityCheck(void* unvefiried_ptr, PUNICODE_STRING name) noexcept;

EXTERN_C NTSTATUS
DriverEntry([[maybe_unused]] PDRIVER_OBJECT driver_object,
            [[maybe_unused]] PUNICODE_STRING registry_path) noexcept {
  // NotAllObjectsAreDestroyed();
  // ExcInDtor();
  // NoexceptViolated();
  // AggregateProblem();      // BSOD
  BufferSecurityCheck(driver_object, registry_path);  // BSOD

  return STATUS_UNSUCCESSFUL;
}

void NotAllObjectsAreDestroyed() noexcept {
  using object_t = ThrowAtNth<2>;
  try {
    [[maybe_unused]] object_t obj1;  // Not destroyed
    [[maybe_unused]] object_t obj2;
  } catch (const Exception&) {
    PRINT_CATCH(ThrowAtNth, 2)
  }
}

void AggregateProblem() noexcept {
  using object_t = ThrowAtNth<4>;
  try {
    /**
     * unresolved external symbol "void __cdecl `eh vector destructor iterator'
     * (void *,unsigned __int64,unsigned __int64,void (__cdecl*)(void))"
     * with the original Avakar's code
     *
     * Ok, let's implement it below
     *
     * Also see /runtime/src/array_unwind.cpp in KTL
     */
    Array<object_t, 4> arr{1, 2, 3};
  } catch (const Exception&) {
    PRINT_CATCH(ThrowAtNth,
                4)  // Unreachable due to fail with nt!__C_specific_handler
  }
}

void ExcInDtor() noexcept {
  using object_t = ThrowInDtor<2>;
  try {
    /**
     * unresolved external symbol "void __cdecl `eh vector constructor iterator'
     * (void *,unsigned __int64,unsigned __int64,void (__cdecl*)(void))"
     * with the original Avakar's code
     *
     * Ok, let's implement it below
     *
     * Also see /runtime/src/array_unwind.cpp in KTL
     */

    [[maybe_unused]] object_t objects[4];
    /**
     * C++ Standart requires a terminate() call
     */
  } catch (const Exception&) {
    PRINT_CATCH(ThrowInDtor, 2)
  }
}

void NoexceptViolated() noexcept {
  using object_t = ThrowAtNth<1>;
  constexpr auto fire{[]() noexcept { [[maybe_unused]] object_t object; }};
  try {
    fire();
  } catch (const Exception&) {
    PRINT_CATCH(ThrowAtNth, 1)
  }
}

void BufferSecurityCheck(void* unverified_ptr, PUNICODE_STRING name) noexcept {
  constexpr unsigned ref{L'\\'};
  try {
    const auto ch{name->Buffer[0]};  // ch is always L'\\'
    if (unverified_ptr) {
      MakeCall<void* (*)(void*)>(name, unverified_ptr);
    } else {
      UNICODE_STRING dummy = RTL_CONSTANT_STRING(L"DummyRoutine");
      MakeCall<void* (*)()>(&dummy);
    }
  } catch (const Exception&) {
    DbgPrint("%s: exception caught!\n", __FUNCTION__);
  }
}

EXTERN_C void __cdecl __ehvec_dtor(void* arr_end,
                                   size_t element_size,
                                   size_t count,
                                   void (*destructor)(void*)) {
  auto* current_obj{static_cast<unsigned char*>(arr_end)};
  while (count-- > 0) {
    current_obj -= element_size;
    destructor(current_obj);
  }
}

EXTERN_C void __cdecl __ehvec_ctor(void* arr_begin,
                                   size_t element_size,
                                   size_t count,
                                   void (*constructor)(void*),
                                   void (*destructor)(void*)) {
  auto* current_obj{static_cast<unsigned char*>(arr_begin)};
  size_t idx{0};

  try {
    for (; idx < count; ++idx) {
      constructor(current_obj);
      current_obj += element_size;
    }
  } catch (...) {  // Original MSVC's implementation uses SEH __finally frame
    __ehvec_dtor(current_obj, element_size, idx, destructor);
#pragma warning(suppress : 4297)  // It's not a production code :)
    throw;
  }
}
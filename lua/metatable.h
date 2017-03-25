// Copyright 2016 Cheng Zhao. All rights reserved.
// Use of this source code is governed by the license that can be found in the
// LICENSE file.

#ifndef LUA_METATABLE_H_
#define LUA_METATABLE_H_

#include <type_traits>

#include "lua/metatable_internal.h"

namespace lua {

// Defines how the wrapper of RefCounted is destructed.
template<typename T>
struct UserData<T, typename std::enable_if<std::is_base_of<
                       base::subtle::RefCountedBase, T>::value>::type> {
  using Type = T*;
  static inline void Construct(State* state, T** data, T* ptr) {
    ptr->AddRef();
    *data = ptr;
  }
  static inline void Destruct(T** data) {
    (*data)->Release();
  }
  static inline T* From(State* state, T** data) {
    return *data;
  }
};

// Defines how the wrapper of WeakPtr is destructed.
template<typename T>
struct UserData<T, typename std::enable_if<std::is_base_of<
                       base::internal::WeakPtrBase,
                       decltype(((T*)nullptr)->GetWeakPtr())>::value>::type> {
  using Type = base::WeakPtr<T>;
  static inline void Construct(State* state, base::WeakPtr<T>* data, T* ptr) {
    new(data) base::WeakPtr<T>(ptr->GetWeakPtr());
  }
  static inline void Destruct(base::WeakPtr<T>* data) {
    data->~Type();
  }
  static inline T* From(State* state, base::WeakPtr<T>* data) {
    return data->get();
  }
};

// Generate metatable for native classes.
template<typename T, typename Enable = void>
struct MetaTable {
};

// Helper to push metatable.
template<typename T>
inline void Push(State* state, MetaTable<T>) {
  internal::InheritanceChain<T>::Push(state);
}

// Check if T's metatable is base of the metatable on top of stack.
template<typename T>
bool IsMetaTableInheritedFrom(State* state) {
  StackAutoReset reset(state);
  base::StringPiece name;
  if (RawGetAndPop(state, -1, "__name", &name) && name == Type<T>::name)
    return true;
  if (!GetMetaTable(state, -1))
    return false;
  RawGet(state, -1, "__index");
  return IsMetaTableInheritedFrom<T>(state);
}

// A helper for creating a new instance of T.
template<typename T, typename... ArgTypes>
T* CreateInstance(State* state, const ArgTypes&... args) {
  StackAutoReset reset(state);
  T* ptr = new T(args...);
  NewUserData(state, ptr);
  Push(state, MetaTable<T>());
  SetMetaTable(state, -2);
  return ptr;
}

// The default type information for RefCounted class.
template<typename T>
struct Type<T*, typename std::enable_if<std::is_base_of<
                    base::subtle::RefCountedBase, T>::value>::type> {
  static constexpr const char* name = Type<T>::name;
  static bool To(State* state, int index, T** out) {
    index = AbsIndex(state, index);
    StackAutoReset reset(state);
    // Verify the type and length.
    if (GetType(state, index) != lua::LuaType::UserData ||
        RawLen(state, index) != sizeof(typename UserData<T>::Type))
      return false;
    // Verify the inheritance chain.
    if (!GetMetaTable(state, index) || !IsMetaTableInheritedFrom<T>(state))
      return false;
    // Convert pointer to actual class.
    *out = UserData<T>::From(
        state, static_cast<T**>(lua_touserdata(state, index)));
    return true;
  }
  static inline void Push(State* state, T* ptr) {
    if (!ptr) {
      lua::Push(state, nullptr);
    } else if (!internal::WrapperTableGet(state, ptr)) {
      NewUserData(state, ptr);
      internal::WrapperTableSet(state, ptr, -1);
      internal::InheritanceChain<T>::Push(state);
      SetMetaTable(state, -2);
    }
  }
};

// The default type information for WeakPtr class.
template<typename T>
struct Type<T*, typename std::enable_if<std::is_base_of<
                    base::internal::WeakPtrBase,
                    decltype(((T*)nullptr)->GetWeakPtr())>::value>::type> {
  static constexpr const char* name = Type<T>::name;
  static bool To(State* state, int index, T** out) {
    index = AbsIndex(state, index);
    StackAutoReset reset(state);
    // Verify the type and length.
    if (GetType(state, index) != lua::LuaType::UserData ||
        RawLen(state, index) != sizeof(typename UserData<T>::Type))
      return false;
    // Convert pointer to actual class.
    T* ptr = UserData<T>::From(
        state, static_cast<base::WeakPtr<T>*>(lua_touserdata(state, index)));
    // WeakPtr might be invalidated.
    if (!ptr)
      return false;
    *out = ptr;
    return true;
  }
  static inline void Push(State* state, T* ptr) {
    if (!ptr)
      lua::Push(state, nullptr);
    NewUserData(state, ptr);
    internal::InheritanceChain<T>::Push(state);
    SetMetaTable(state, -2);
  }
};

}  // namespace lua

#endif  // LUA_METATABLE_H_

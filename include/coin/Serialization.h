/* -*-c++-*- libcoin - Copyright (C) 2012 Michael Gronager
 *
 * libcoin is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * libcoin is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with libcoin.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef SERIALIZATION_H
#define SERIALIZATION_H

#include <vector>
#include <set>
#include <map>
#include <iostream>
#include <stdint.h>

#include <boost/type_traits.hpp>

/// Serialization
/// Serialization for bitcoin is pretty simple:
/// * basic types are serialized as their memory representation on a little endian system.
/// ** use const_binary<>() to write basic types
/// ** use binary<>() read basic types
/// * size data can be serialized as varints - to optimize space
/// ** use const_varint() to write varints
/// ** use varint() to read varints
/// * container types are serialized as a "varint" followed by the elements
/// ** container types can be serialized and deserialized directly
/// * strings are considered containertypes and are serialized as varstr:
/// ** use const_varstr() to write
/// ** use varstr() to read
/// all other serialization are composed of the elements above.
/// example:
///   os << const_binary<int>(version) << const_varstr(version_string) << vector_data
///   is >> binary<int>(version) >> varstr(version_string) >> vector_data
/// Where os is a std::ostream and is is a std::istream.
/// Use of the standard friend ostream& operator<<(ostream&, const Object& object) and
/// its istream counter part is recommended for all classes.

template <typename T>
class binary {
public:
    binary(T& t) : _t(t) {}
    
    template <typename TT>
    friend std::istream& operator>>(std::istream& is, const binary<TT>& bin);
private:
    T& _t;
};

template <typename T>
std::istream& operator>>(std::istream& is, const binary<T>& bin) {
    return is.read((char*)&bin._t, sizeof(T));
}

template <typename T>
class const_binary {
public:
    const_binary(const T& t) : _t(t) {}
    
    template <typename TT>
    friend std::ostream& operator<<(std::ostream& os, const const_binary<TT>& bin);
    
private:
    const T& _t;
};

template <typename T>
std::ostream& operator<<(std::ostream& os, const const_binary<T>& bin) {
    return os.write((const char*)&bin._t, sizeof(T));
}

class const_varint {
public:
    const_varint(const uint64_t& var_int) : _int(var_int) {}
    
    friend std::ostream& operator<<(std::ostream& os, const const_varint& var);
    
private:
    const uint64_t& _int;
};

class varint {
public:
    varint(uint64_t& var_int) : _int(var_int) {}
    
    friend std::istream& operator>>(std::istream& is, const varint& var);
private:
    uint64_t& _int;
};

class const_varstr {
public:
    const_varstr(const std::string& str) : _str(str) {}
    
    friend std::ostream& operator<<(std::ostream& os, const const_varstr& var);
    
private:
    const std::string& _str;
};

class varstr {
public:
    varstr(std::string& str) : _str(str) {}
    
    friend std::istream& operator>>(std::istream& is, const varstr& var);
private:
    std::string& _str;
};

template <class T>
std::ostream& operator<<(std::ostream& os, const std::vector<T>& array) {
    os << const_varint(array.size());
    for (typename std::vector<T>::const_iterator t = array.begin(); t != array.end(); ++t) {
        if (boost::is_class<T>())
            os << *t;
        else
            os << const_binary<T>(*t);
    }
    return os;
}

template <class T>
std::istream& operator>>(std::istream& is, std::vector<T>& array) {
    uint64_t size = 0;
    is >> varint(size);
    array.resize(size);
    for (typename std::vector<T>::iterator t = array.begin(); t != array.end(); ++t) {
        if (boost::is_class<T>())
            is >> *t;
        else
            is >> binary<T>(*t);
    }
    return is;
}

template <class T>
std::ostream& operator<<(std::ostream& os, const std::set<T>& array) {
    os << const_varint(array.size());
    for (typename std::set<T>::const_iterator t = array.begin(); t != array.end(); ++t) {
        if (boost::is_class<T>())
            os << *t;
        else
            os << const_binary<T>(*t);
    }
    return os;
}

template <class T>
std::istream& operator>>(std::istream& is, std::set<T>& array) {
    uint64_t size = 0;
    is >> varint(size);
    for (size_t i = 0; i < size; ++i) {
        T t;
        if (boost::is_class<T>())
            is >> t;
        else
            is >> binary<T>(t);
        array.insert(t);
    }
    return is;
}

template <typename F, typename S>
std::ostream& operator<<(std::ostream& os, const std::pair<F, S>& p) {
    if (boost::is_class<F>())
        os << p.first;
    else
        os << const_binary<F>(p.first);
    if (boost::is_class<S>())
        return os << p.second;
    else
        return os << const_binary<S>(p.second);
}

template <typename F, typename S>
std::istream& operator>>(std::istream& is, std::pair<F, S>& p) {
    if (boost::is_class<F>())
        is >> p.first;
    else
        is >> binary<F>(p.first);
    if (boost::is_class<S>())
        return is >> p.second;
    else
        return is >> binary<S>(p.second);
}

template <typename K, typename V>
std::ostream& operator<<(std::ostream& os, const std::map<K, V>& container) {
    os << const_varint(container.size());
    for (typename std::map<K, V>::const_iterator c = container.begin(); c != container.end(); ++c) {
        os << (*c);
    }
    return os;
}

template <typename K, typename V>
std::istream& operator>>(std::istream& is, std::map<K, V>& container) {
    container.clear();
    uint64_t size = 0;
    is >> varint(size);
    typename std::map<K, V>::iterator m = container.begin();
    for (size_t i = 0; i < size; ++i) {
        std::pair<K, V> p;
        is >> p;
        m = container.insert(m, p);
    }
    return is;
}

template <typename T>
std::string serialize(const T& t) {
    std::ostringstream os;
    if (boost::is_class<T>())
        os << t;
    else
        os << const_binary<T>(t);
    std::string s = os.str();
    return s;//os.str();
}

inline std::string serialize(const std::string& t) {
    std::ostringstream os;
    os << const_varstr(t);
    return os.str();
}

template <typename T>
std::size_t serialize_size(const T& t) {
    return serialize(t).size();
}

template <typename T>
T deserialize(const std::string& s) {
    std::istringstream is(s);
    T t;
    if (boost::is_class<T>())
        is >> t;
    else
        is >> binary<T>(t);
    return t;
}

inline std::string deserialize(const std::string& s) {
    std::istringstream is(s);
    std::string t;
    is >> varstr(t);
    return t;
}

#endif // SERIALIZATION_H
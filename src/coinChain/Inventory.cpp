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

#include <coinChain/Inventory.h>
#include <coin/Transaction.h>
#include <coin/Block.h>
//#include <coinChain/MerkleBlock.h>

#include <coin/util.h>

#include <boost/assign/list_of.hpp>

using namespace std;
using namespace boost;

map<int, string> Inventory::known_types = assign::map_list_of
    ( 0, "ERROR")
    ( (int)MSG_TX, "tx")
    ( (int)MSG_BLOCK, "block")
    ( (int)MSG_FILTERED_BLOCK, "filtered block")
    ( (int)MSG_NORMALIZED_TX, "normalized tx")
    ( (int)MSG_NORMALIZED_BLOCK, "normalized block");


Inventory::Inventory() {
    _type = 0;
    _hash = 0;
}

Inventory::Inventory(int type, const uint256& hash) {
    _type = type;
    _hash = hash;
}

Inventory::Inventory(const Block& blk) {
    _type = MSG_BLOCK;
    _hash = blk.getHash();
}

Inventory::Inventory(const Transaction& txn) {
    _type = MSG_TX;
    _hash = txn.getHash();
}

bool operator<(const Inventory& a, const Inventory& b) {
    return (a._type < b._type || (a._type == b._type && a._hash < b._hash));
}

bool Inventory::isKnownType() const {
    std::map<int, string>::const_iterator it = known_types.find(_type);
    return (_type > 0) && (it != known_types.end());
}

std::string Inventory::toString() const {
    if (!isKnownType())
        throw std::out_of_range(cformat("Inventory::GetCommand() : type=%d unknown type", _type).text());

    return cformat("%s %s", known_types[_type], _hash.toString().substr(0,20)).text();
}

/**
*** Copyright (c) 2016-present,
*** Jaguar0625, gimre, BloodyRookie, Tech Bureau, Corp. All rights reserved.
***
*** This file is part of Catapult.
***
*** Catapult is free software: you can redistribute it and/or modify
*** it under the terms of the GNU Lesser General Public License as published by
*** the Free Software Foundation, either version 3 of the License, or
*** (at your option) any later version.
***
*** Catapult is distributed in the hope that it will be useful,
*** but WITHOUT ANY WARRANTY; without even the implied warranty of
*** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
*** GNU Lesser General Public License for more details.
***
*** You should have received a copy of the GNU Lesser General Public License
*** along with Catapult. If not, see <http://www.gnu.org/licenses/>.
**/

#pragma once
#include <memory>

namespace catapult { namespace model { class TransactionRegistry; } }

namespace catapult { namespace api {

	/// Base for traits that depend on a transaction registry.
	/// \note This is an implementation detail that is tested indirectly.
	template<typename TEntity>
	struct RegistryDependentTraits {
	public:
		/// Creates traits around \a registry.
		explicit RegistryDependentTraits(const model::TransactionRegistry& registry) : m_registry(registry)
		{}

	public:
		/// Returns \c true if \a entity passes size checks.
		bool operator()(const TEntity& entity) {
			return IsSizeValid(entity, m_registry);
		}

	private:
		const model::TransactionRegistry& m_registry;
	};
}}

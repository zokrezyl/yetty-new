#pragma once

#include <yetty/core/factory.hpp>
#include <yetty/core/object.hpp>

namespace yetty {
namespace core {

// FactoryObject - convenience base for "big objects" that need both
// identity (Object) and controlled creation (ObjectFactory).
//
// Usage:
//   class Widget : public FactoryObject<Widget> {
//   public:
//       static Result<Ptr> createImpl();
//       virtual void doSomething() = 0;
//   };
//
template <typename T>
class FactoryObject : public Object, public core::ObjectFactory<T> {
public:
  using Ptr = std::shared_ptr<T>;
};

} // namespace core
} // namespace yetty

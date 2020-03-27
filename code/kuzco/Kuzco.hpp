#pragma once

#include <memory>
#include <vector>
#include <mutex>
#include <type_traits>

namespace kuzco
{
template <typename T>
class RootObject;

namespace impl
{

// a unit of state information
struct Data
{
    using Payload = std::shared_ptr<void>;

    void* qdata = nullptr; // quick access pointer to save dereferencs of the internal shared pointer
    Payload payload;

    template <typename T, typename... Args>
    static Data construct(Args&&... args)
    {
        Data ret;
        ret.payload = std::make_shared<T>(std::forward<Args>(args)...);
        ret.qdata = ret.payload.get();
        return ret;
    }
};

class RootObject;

// base class for new objects
class NewObject
{
protected:
    NewObject();

    // we open and close each access to the new object
    // that's why we need a set function which is separate from the constructor
    // the child class will invoke the appropriate constrcutor in its body
    void setData(Data d);

    // explicit functions to call when writing to data
    void openDataEdit();
    void closeDataEdit();

    Data m_data;

    friend class Member;
};

// base class for members
class Member
{
protected:
    Member();
    ~Member();

    void takeData(Member& other);
    void takeData(NewObject& other);

    // check if we're in a deep or shallow copy state
    // we're deep when working with new objects
    // and shallow when we're working with state objects within a transaction
    static bool deep();

    // if the we're working on new objects we're detached
    // if we're inisde a transaction a search is performed in order to save copies
    bool detached() const;

    // deaches the object with a new data
    // only valid if not detached
    void detachWith(Data data);

    // perform the detached check
    // detach if needed
    // reassign data from other source
    void checkedDetachTake(Member& other);
    void checkedDetachTake(NewObject& other);

    Data m_data;

    friend class RootObject;

    template <typename T>
    friend class kuzco::RootObject;
};

class RootObject
{
protected:
    RootObject(NewObject&& obj) noexcept;

    void beginTransaction();
    void endTransaction();

    // safe even during a transaction
    Data::Payload detachedRoot() const;

    Member m_root;

private:
    friend class Member;

    std::mutex m_transactionMutex;

    // check if the data is in the open edits
    bool isOpenEdit(const Data& d) const;
    void openEdit(const Data& d);
    std::vector<Data> m_openEdits;

    Data::Payload m_detachedRoot; // transaction safe root, atomically updated only after transaction ends
};

} // namespace impl

template <typename T>
class NewObject : public impl::NewObject
{
public:
    template <typename... Args>
    NewObject(Args&&... args)
    {
        setData(impl::Data::construct<T>(std::forward<Args>(args)...));
    }

    struct Write
    {
        Write(NewObject& o) : m_object(o) { o.openDataEdit(); }
        NewObject& m_object;
        T* operator->() { return reinterpret_cast<T*>(m_object.m_data.qdata); }
        ~Write() { m_object.closeDataEdit(); }
    };

    Write w() { return Write(*this); };

    const T* get() const { return reinterpret_cast<const T*>(m_data.qdata); }
    const T* operator->() const { return get(); }
    const T& operator*() const { return *get(); }

    std::shared_ptr<const T> payload() const { return std::reinterpret_pointer_cast<const T>(m_data.payload); }
};

template <typename T>
class Member : public impl::Member
{
public:
    template <typename... Args, std::enable_if_t<std::is_constructible_v<T, Args...>, int> = 0>
    Member(Args&&... args)
    {
        m_data = impl::Data::construct<T>(std::forward<Args>(args)...);
    }

    Member(const Member& other)
    {
        if (deep()) m_data = impl::Data::construct<T>(*other.get());
        else m_data = other.m_data;
    }

    Member& operator=(const Member& other)
    {
        if (detached()) *qget() = *other.get();
        else detachWith(impl::Data::construct<T>(*other.get()));
        return *this;
    }

    Member(Member&& other) noexcept { takeData(other); }
    Member& operator=(Member&& other) { checkedDetachTake(other); return *this; }

    Member(NewObject<T>&& obj) noexcept { takeData(obj); }
    Member& operator=(NewObject<T>&& obj) { checkedDetachTake(obj); return *this; }

    template <typename U, std::enable_if_t<std::is_assignable_v<T&, U>, int> = 0>
    Member& operator=(U&& u)
    {
        if (detached()) *qget() = std::forward<U>(u);
        else detachWith(impl::Data::construct<T>(std::forward<U>(u)));
        return *this;
    }

    const Member& r() const { return *this; }
    const T* get() const { return reinterpret_cast<const T*>(m_data.qdata); }
    const T* operator->() const { return get(); }
    const T& operator*() const { return *get(); }

    T* get()
    {
        if (!detached()) detachWith(impl::Data::construct<T>(*r().get()));
        return qget();
    }
    T* operator->() { return get(); }
    T& operator*() { return *get(); }

    std::shared_ptr<const T> payload() const { return std::reinterpret_pointer_cast<const T>(m_data.payload); }

private:
    T* qget() { return reinterpret_cast<T*>(m_data.qdata); }
};

template <typename T>
class RootObject : public impl::RootObject
{
public:
    RootObject(NewObject<T>&& obj) : impl::RootObject(std::move(obj)) {}

    RootObject(const RootObject&) = delete;
    RootObject& operator=(const RootObject&) = delete;
    RootObject(RootObject&&) = delete;
    RootObject& operator=(RootObject&&) = delete;

    // returns a non-const pointer to the underlying data
    T* beginTransaction()
    {
        impl::RootObject::beginTransaction();
        m_root.detachWith(impl::Data::construct<T>(*reinterpret_cast<const T*>(m_root.m_data.qdata)));
        return reinterpret_cast<T*>(m_root.m_data.qdata);
    }

    void endTransaction() { impl::RootObject::endTransaction(); }

    std::shared_ptr<const T> detachedPayload() const { return std::reinterpret_pointer_cast<const T>(detachedRoot()); }
};

} // namespace kuzco

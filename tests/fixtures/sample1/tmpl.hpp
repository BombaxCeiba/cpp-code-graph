// sample1 fixture: primary class template, partial specialization, and a
// function template; bodies call the function template so calls inside
// instantiated bodies land on primary-template symbols. Parse-input DATA.
#ifndef SAMPLE1_TMPL_HPP
#define SAMPLE1_TMPL_HPP

namespace shape {

template <typename T>
T unwrap(T value)
{
    return value;
}

template <typename T>
struct Box {
    T value;

    T get() const { return unwrap(value); }
};

template <typename T>
struct Box<T*> {
    T* value;

    T* get() const { return unwrap(value); }
};

}  // namespace shape

#endif

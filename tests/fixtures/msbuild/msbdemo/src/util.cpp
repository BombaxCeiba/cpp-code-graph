#include "util.hpp"

#include <demo/answer.hpp> // found via AdditionalIncludeDirectories "include"

#include "config.hpp"

namespace demo
{

    int add(int a, int b) { return a + b; }

    const std::string& mode_name()
    {
        static const std::string name = banner();
        return name;
    }

    int answer() { return demo_detail::the_answer(); }

} // namespace demo

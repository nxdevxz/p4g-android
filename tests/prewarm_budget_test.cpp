#include "nvn/prewarm_budget.h"
#include <cassert>
int main() {
    nx::nvn::PrewarmBudget budget;
    budget.begin(7);
    assert(budget.available(0, 250000, 1));
    ++budget.started;
    budget.begin(7); // a second submission must not restart the frame budget
    assert(!budget.available(0, 250000, 1));
    assert(budget.available(0, 1000000, 3));
    budget.spent_ns = 200000;
    assert(!budget.available(50000, 250000, 3));
    budget.begin(8);
    assert(budget.started == 0 && budget.spent_ns == 0);
    assert(budget.available(249999, 250000, 1));
}

#include <flux/flux.hh>

using namespace flux;
using namespace std;

void Graph::toDot(std::ostream& o) const
{
  // TODO plot something to highlight relations

  o << "digraph {" << endl;

  for (auto const& domain : _domains)
  {
    o << "  subgraph cluster_" << domain->getName() << " {" << endl;
    o << "    label=" << domain->getName() << ";" << endl;
    o << "    graph [style=dotted];" << endl;

    for (auto const& field : domain->getFields())
    {
      o << "    \"" << field->getName() << "\"";
      if (dynamic_cast<ComputedFieldBase*>(field.get()) != nullptr)
        o << " [shape=box]";
      o << ";" << endl;
    }

    o << "  }" << endl;
  }

  for (auto const& domain : _domains)
  {
    for (auto const& field : domain->getFields())
    {
      FieldBase* f = field.get();
      auto computedField = dynamic_cast<ComputedFieldBase*>(f);
      if (computedField != nullptr)
      {
        for (auto const& dep : computedField->getDependencies())
          o << "  \"" << dep->getName() << "\" -> \"" << f->getName() << "\";" << endl;
      }
    }
  }

  o << "}" << endl;
}

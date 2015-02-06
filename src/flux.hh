#pragma once

#include <cassert>
#include <map>
#include <memory>
#include <deque>
#include <set>
#include <sstream>

#include <camshaft/any.hh>
#include <camshaft/demangle.hh>
#include <camshaft/graphs.hh>
#include <camshaft/memory.hh>
#include <camshaft/uuid.hh>

//static std::ostream& operator<<(std::ostream& s, any a)
//{
//  if (a.is<std::string>())
//    s << any_cast<std::string>(a);
//  else if (a.is<int>())
//    s << any_cast<int>(a);
//  else if (a.is<double>())
//    s << any_cast<double>(a);
//  else if (a.is<Uuid>())
//    s << any_cast<Uuid>(a);
//  else
//    s << "(unknown " << a.type().name() << ")";
//
//  return s;
//}

namespace flux
{
  template<typename> class Domain;
  class ComputedFieldBase;
  class DomainBase;

  class FieldBase
  {
  public:
    explicit FieldBase(std::string name)
      : _name(name)
    {}

    virtual ~FieldBase() = default;

    virtual DomainBase& getDomain() const = 0;
    virtual any getValue(const any& key) const = 0;
    virtual void addDependant(ComputedFieldBase* dependant) = 0;

    std::string getName() const { return _name; }

    virtual void visit(std::function<void(const std::pair<any,any>&)>) = 0;

    virtual std::function<void()> subscribe(std::function<void(const any&,const any&)> callback) = 0;

  private:
    std::string _name;
  };

  class Params
  {
  public:
    Params(std::map<const DomainBase*,any> keyByDomain,
           std::map<const FieldBase*,any> valueByField)
    : _keyByDomain(keyByDomain),
      _valueByField(valueByField)
    {}

    template<typename TField>
    typename TField::ValueType operator()(const TField& field) const
    {
      auto it = _valueByField.find(&field);
      assert(it != _valueByField.end());
      assert(!it->second.empty());
      return any_cast<typename TField::ValueType>(it->second);
    }

    template <typename TField>
    const typename TField::KeyType& key(const TField& field) const
    {
      auto it = _keyByDomain.find(&field.getDomain());
      assert(it != _keyByDomain.end());
      return any_cast<typename TField::KeyType>(it->second);
    }

  private:
    Params(const Params&) = delete;
    Params& operator=(const Params&) = delete;

    std::map<const DomainBase*,any> _keyByDomain;
    std::map<const FieldBase*,any> _valueByField;
  };

  template<typename TValue, typename TKey>
  class TypedFieldBase : public virtual FieldBase
  {
  public:
    typedef TValue ValueType;
    typedef TKey KeyType;

    TypedFieldBase(std::string name, Domain<TKey>& domain)
      : FieldBase(name),
        _domain(domain),
        _valueByKey(),
        _observers(),
        _dependantComputations()
    {}

    ~TypedFieldBase() override = default;

    std::function<void()> subscribe(std::function<void(const any&,const any&)> observer) override
    {
      return subscribe([observer](const TKey& key,const TValue& value)
      {
        any anyKey = key;
        any anyValue = value;
        observer(anyKey, anyValue);
      });
    }

    std::function<void()> subscribe(std::function<void(const TKey&,const TValue&)> observer)
    {
      // Give each subscription an ID. This allows removal of the subscription later.
      // This is because std::function is not comparable.
      static ulong nextObserverId = 0;
      ulong observerId = nextObserverId++;

      _observers.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(observerId),
        std::forward_as_tuple(observer));

      // Return a function that cancels the subscription when invoked
      return [this,observer,observerId]
      {
        size_t removedCount = _observers.erase(observerId);
        assert(removedCount == 1);
      };
    }

    DomainBase& getDomain() const override
    {
      return _domain;
    }

    void addDependant(ComputedFieldBase* dependant) override
    {
      _dependantComputations.insert(dependant);
    }

    std::set<ComputedFieldBase*>& getDependants() { return _dependantComputations; }

    virtual void setValue(const TKey& key, const TValue& value)
    {
      // Store new value
      _valueByKey[key] = value;

      // If any computed properties depend upon this, set 'computation required' and store relevant data
      if (!_dependantComputations.empty())
        _domain.onComputationInputChanged(*this, key);

      // If any clients have subscribed, set 'publish required' and store relevant data
      if (_observers.size())
        _domain.addPublishTask([=] { notifyObservers(key, value); });
    }

    TValue getValue(const TKey key) const
    {
      auto it = find(key);
      if (it == end())
        throw std::runtime_error("No value exists for key");
      return it->second;
    }

    any getValue(const any& key) const override
    {
      const TKey& k = any_cast<TKey>(key);
      auto it = find(k);
      return it == end() ? any() : it->second;
    }

    typename std::map<TKey,TValue>::const_iterator find(const TKey& key) const
    {
      return _valueByKey.find(key);
    }

    typename std::map<TKey,TValue>::const_iterator begin() const { return _valueByKey.begin(); }

    typename std::map<TKey,TValue>::const_iterator end() const { return _valueByKey.end(); }

    size_t count() const { return _valueByKey.size(); }

    void notifyObservers(const TKey& key, const TValue& value)
    {
      for (auto& pair : _observers)
        pair.second(key, value);
    }

    void visit(std::function<void(const std::pair<any,any>&)> visitor) override
    {
      for (auto const& pair : _valueByKey)
      {
        std::pair<any,any> anyPair(pair.first, pair.second);
        visitor(anyPair);
      }
    }

  private:
    TypedFieldBase(const TypedFieldBase&) = delete;
    TypedFieldBase& operator=(const TypedFieldBase&) = delete;

    Domain<TKey>& _domain;
    std::map<TKey,TValue> _valueByKey;
    std::map<ulong,std::function<void(const TKey&,const TValue&)>> _observers;
    std::set<ComputedFieldBase*> _dependantComputations;
  };

  class ComputedFieldBase
  {
  public:
    virtual ~ComputedFieldBase() = default;

    virtual DomainBase& getDomain() const = 0;

    virtual bool recalculate(const any& key) = 0;

    const std::set<FieldBase*>& getDependencies() const { return _dependencies; }

  protected:
    explicit ComputedFieldBase(std::set<FieldBase*> dependencies)
      : _dependencies(dependencies)
    {}

    std::set<FieldBase*> _dependencies;
  };

  template<typename TValue, typename TKey>
  class Field : public TypedFieldBase<TValue, TKey>
  {
  public:
    Field(std::string name, Domain<TKey>& domain)
      : FieldBase(name),
        TypedFieldBase<TValue, TKey>(name, domain)
    {}

    ~Field() override = default;

  private:
    Field(const Field&) = delete;
    Field& operator=(const Field&) = delete;
  };

  class RelationFieldBase : virtual public FieldBase
  {
  public:
    explicit RelationFieldBase(std::string name)
      : FieldBase(name)
    {}

    virtual std::vector<any> getKeys(any const& remoteKey) const = 0;

    virtual DomainBase& getRemoteDomain() const = 0;

  private:
    RelationFieldBase(const RelationFieldBase&) = delete;
    RelationFieldBase& operator=(const RelationFieldBase&) = delete;
  };

  template<typename TKeyLocal, typename TKeyRemote>
  class RelationField : public RelationFieldBase, public TypedFieldBase<TKeyRemote, TKeyLocal>
  {
  public:
    RelationField(std::string name, Domain<TKeyLocal>& localDomain, Domain<TKeyRemote>& remoteDomain)
      : FieldBase(name),
        RelationFieldBase(name),
        TypedFieldBase<TKeyRemote, TKeyLocal>(name, localDomain),
        _remoteDomain(remoteDomain),
        _localKeysByRemoteKey()
    {}

    std::vector<any> getKeys(any const& remoteKey) const override
    {
      assert(!remoteKey.empty());
      assert(remoteKey.is<TKeyRemote>());

      std::vector<any> keys;
      auto it = _localKeysByRemoteKey.equal_range(any_cast<TKeyRemote>(remoteKey));
      for (auto i = it.first; i != it.second; i++)
        keys.emplace_back(i->second);
      return std::move(keys);
    }

    DomainBase& getRemoteDomain() const override
    {
      return _remoteDomain;
    }

    void setValue(TKeyLocal const& key, TKeyRemote const& value) override
    {
      TypedFieldBase<TKeyRemote, TKeyLocal>::setValue(key, value);
      _localKeysByRemoteKey.insert(std::make_pair(value, key));
    }

  private:
    Domain<TKeyRemote>& _remoteDomain;
    std::multimap<TKeyRemote, TKeyLocal> _localKeysByRemoteKey;
  };

  class DomainBase
  {
  public:
    explicit DomainBase(std::string name)
      : _name(name)
    {}

    virtual ~DomainBase() = default;

    virtual bool isComputeRequired() const = 0;
    virtual bool isPublishRequired() const = 0;
    virtual void compute() = 0;
    virtual void publish() = 0;

    /// Gets a vector containing a sequence of foreign keys that may be followed in order
    /// to traverse from this domain to a related domain. If the vector is empty, then no
    /// such path exists.
    virtual const std::vector<RelationFieldBase*>& getRelationPathTo(const DomainBase& relatedDomain) = 0;
    virtual any getRelatedKey(any key, const DomainBase& relatedDomain) = 0;
    virtual void addComputeTask(std::function<void()>&& callback) = 0;
    virtual const std::vector<std::unique_ptr<FieldBase>>& getFields() const = 0;
    virtual const std::vector<RelationFieldBase*> getForeignKeys() const = 0;

    std::string getName() const { return _name; }

    FieldBase* findField(std::string fieldName) const
    {
      // TODO this could be O(1) instead of O(N) if we want to manage another map
      for (auto const& field : getFields())
        if (field->getName() == fieldName)
          return field.get();
      return nullptr;
    }

  private:
    std::string _name;
  };

  template<typename TValue, typename TKey>
  class ComputedField
    : public ComputedFieldBase,
      public TypedFieldBase<TValue, TKey>
  {
  public:
    ComputedField(
      std::string name,
      Domain<TKey>& domain,
      std::set<FieldBase*> dependencies,
      std::function<TValue(const Params&)> calculation)
      : FieldBase(name),
        ComputedFieldBase(dependencies),
        TypedFieldBase<TValue, TKey>(name, domain),
        _calculation(calculation)
      {}

    ~ComputedField() override = default;

    bool recalculate(const any& key) override
    {
      assert(!key.empty());
      assert(key.is<TKey>());

//      std::cout << "  recalculate " << getDomain().getName() << "::" << this->getName() << " (computed)" << std::endl;

      std::map<DomainBase const*, any> keyByDomain {{&getDomain(), key}};
      std::map<FieldBase const*, any> valueByField;

      for (FieldBase* dependency : _dependencies)
      {
//        std::cout << "    dependency: " << dependency->getDomain().getName() << "::" << dependency->getName() << std::endl;

        //
        // Find dependency's key
        //

        any dependencyKey;
        auto keyIt = keyByDomain.find(&dependency->getDomain());
        if (keyIt == keyByDomain.end())
        {
          // dependant's key not found -- attempt to find it via a relation
          dependencyKey = getDomain().getRelatedKey(key, dependency->getDomain());
          if (dependencyKey.empty())
          {
//            std::cout << "      dependencyKey not found for " << dependency->getName() << std::endl;
            return false;
          }
//          else
//          {
//            std::cout << "      dependencyKey found for " << dependency->getName() << " (related domain): " << dependencyKey << std::endl;
//          }
        }
        else
        {
          dependencyKey = keyIt->second;
//          std::cout << "      dependencyKey found for " << dependency->getName() << " (same domain): " << dependencyKey << std::endl;
        }

        //
        // Find dependency's value
        //

        any dependencyValue = dependency->getValue(dependencyKey);
        if (dependencyValue.empty())
        {
//          std::cout << "      dependencyValue not found for " << dependency->getName() << std::endl;
          return false;
        }
//        else
//        {
//          std::cout << "      dependencyValue found: " << dependencyValue << std::endl;
//        }

        //
        // Store the dependency's value
        //
        auto insertResult = valueByField.insert(std::make_pair(dependency, dependencyValue));
        assert(insertResult.second); // value should not have previously existed
      }

      // TODO avoid copying these maps into the lambda -- need C++14-style move, or functor or similar
      getDomain().addComputeTask([this, key, keyByDomain, valueByField]
      {
        Params params(keyByDomain, valueByField);
        const TValue val = _calculation(params);
        this->TypedFieldBase<TValue,TKey>::setValue(any_cast<TKey>(key), val);
      });

      return true;
    }

    DomainBase& getDomain() const override
    {
      return TypedFieldBase<TValue, TKey>::getDomain();
    }

  private:
    ComputedField(const ComputedField&) = delete;
    ComputedField& operator=(const ComputedField&) = delete;

    std::function<TValue(const Params&)> _calculation;
  };

  template<typename TKey>
  class Domain : public DomainBase
  {
  public:
    explicit Domain(std::string name)
      : DomainBase(name)
    {}

    ~Domain() override = default;

    template<typename TValue>
    Field<TValue,TKey>& createField(std::string name)
    {
      auto field = std::make_unique<Field<TValue,TKey>>(name, *this);
      auto ptr = field.get();
      _fields.push_back(std::move(field));
      return *ptr;
    }

    /** Create a relation (foreign key) field in this domain, the values of which are keys in the specified remote domain. */
    template<typename TRemoteKey>
    RelationField<TKey,TRemoteKey>& createRelationTo(Domain<TRemoteKey>& remoteDomain)
    {
      assert(reinterpret_cast<void*>(&remoteDomain) != this);
      std::stringstream name;
      name << getName() << "->" << remoteDomain.getName();
      auto field = std::make_unique<RelationField<TKey,TRemoteKey>>(name.str(), *this, remoteDomain);
      auto ptr = field.get();
      _foreignKeys[&remoteDomain] = field.get();
      _fields.push_back(std::move(field));
      return *ptr;
    }

    template<typename TValue>
    void onComputationInputChanged(TypedFieldBase<TValue,TKey>& changedField, const any& key)
    {
      assert(!key.empty() && key.is<TKey>());
      assert(&changedField.getDomain() == this);

//      std::cout << "changed: " << changedField.getName() << " key=" << key << std::endl;

      // Recalculate all computed fields that registered themselves as dependants of the field that changed
      for (auto computedField : changedField.getDependants())
      {
        if (&computedField->getDomain() == this)
        {
          computedField->recalculate(key);
        }
        else
        {
          // The dependant field is in another domain.
          DomainBase& remoteDomain = computedField->getDomain();

          const std::vector<RelationFieldBase*>& relationPath = remoteDomain.getRelationPathTo(*this);

          assert(relationPath.size() != 0);

          if (relationPath.size() == 1)
          {
            // Only one step away
            RelationFieldBase* relationField = relationPath[0];
            // There may be *many* keys in that domain to recompute.
            auto remoteKeys = relationField->getKeys(key);
            for (auto relatedKey : remoteKeys)
              computedField->recalculate(relatedKey);
          }
          else
          {
            // Multiple steps away
            std::vector<any> expandKeys {key};
            for (RelationFieldBase* relationField : relationPath)
            {
              std::vector<any> allRemoteKeys;
              for (any expandKey : expandKeys)
              {
                auto remoteKeys = relationField->getKeys(expandKey);
                allRemoteKeys.insert(allRemoteKeys.end(), remoteKeys.begin(), remoteKeys.end());
              }
              expandKeys = allRemoteKeys;
            }
            for (auto relatedKey : expandKeys)
              computedField->recalculate(relatedKey);
          }
        }
      }
    }

    /** Creates a new computed field. */
    template<typename TValue>
    ComputedField<TValue, TKey>& compute(std::string name, std::set<FieldBase*> fields, std::function<TValue(const Params&)> calculation)
    {
      auto ptr = new ComputedField<TValue,TKey>(name, *this, fields, calculation);
      _fields.emplace_back(ptr);

      std::set<DomainBase*> domains {this};

      // Set the computed field as a dependant of all listed fields
      for (auto& field : fields)
      {
        field->addDependant(ptr);
        domains.insert(&field->getDomain());
      }

      // Also set any involved foreign key fields as dependants
      for (auto d1 : domains)
      for (auto d2 : domains)
      {
        if (d1 == d2)
          continue;
        for (auto fk : d1->getRelationPathTo(*d2))
          fk->addDependant(ptr);
      }

      return *ptr;
    }

    void addPublishTask(std::function<void()>&& task)
    {
      _publishTasks.push_back(std::move(task));
    }

    void addComputeTask(std::function<void()>&& task) override
    {
      _computeTasks.push_back(std::move(task));
    }

    bool isComputeRequired() const override
    {
      return !_computeTasks.empty();
    }

    bool isPublishRequired() const override
    {
      return !_publishTasks.empty();
    }

    void compute() override
    {
      for (auto& task : _computeTasks)
        task();
      _computeTasks.clear();
    }

    void publish() override
    {
      for (auto& task : _publishTasks)
        task();
      _publishTasks.clear();
    }

    const std::vector<RelationFieldBase*> getForeignKeys() const override
    {
      std::vector<RelationFieldBase*> fks;
      for (auto const& pair : _foreignKeys)
        fks.push_back(pair.second);
      return fks;
    }

    const std::vector<RelationFieldBase*>& getRelationPathTo(const DomainBase& relatedDomain)
    {
      DomainBase* relatedPtr = const_cast<DomainBase*>(&relatedDomain);

      // See if we have a cached result
      auto cached = _relationPaths.find(relatedPtr);
      if (cached != _relationPaths.end())
        return cached->second;

      // No cached result, so create one...

      // Test whether the related domain is a direct relation
      auto direct = _foreignKeys.find(relatedPtr);
      if (direct != _foreignKeys.end())
      {
        std::vector<RelationFieldBase*> keys {direct->second};
        auto res = _relationPaths.emplace(relatedPtr, std::move(keys));
        assert(res.second);
        return res.first->second;
      }

      // The relationship is not direct, so search for an indirect one

      auto path = findPath<DomainBase*,RelationFieldBase*>(this, relatedPtr,
        [](DomainBase* d)
        {
          std::queue<std::pair<RelationFieldBase*,DomainBase*>> edges;
          for (auto fk : d->getForeignKeys())
            edges.emplace(fk, &fk->getRemoteDomain());
          return edges;
        });

      // Return just a vector of the relation fields
      std::vector<RelationFieldBase*> fkPath;
      for (auto const& p : path)
        fkPath.push_back(p.first);

      // Memoize (move into cache)
      auto res = _relationPaths.emplace(relatedPtr, move(fkPath));
      assert(res.second);

      // Return reference to cached version
      return res.first->second;
    }

    const std::vector<std::unique_ptr<FieldBase>>& getFields() const override { return _fields; }

    any getRelatedKey(any key, const DomainBase& relatedDomain) override
    {
      assert(!key.empty());
      assert(key.is<TKey>());

      auto fks = getRelationPathTo(relatedDomain);

      if (fks.empty())
        return any();

      for (auto fk : fks)
      {
        key = fk->getValue(key);
//        if (key.empty())
//          std::cout << "Key " << key << " produced no value in FK " << fk->getName() << std::endl;
      }

      return key;
    }

  private:
    Domain(const Domain&) = delete;
    Domain& operator=(const Domain&) = delete;

    std::vector<std::unique_ptr<FieldBase>> _fields;
    std::vector<std::function<void()>> _publishTasks;
    std::vector<std::function<void()>> _computeTasks;
    std::map<DomainBase*,RelationFieldBase*> _foreignKeys;
    std::map<DomainBase*,std::vector<RelationFieldBase*>> _relationPaths;
  };

  class Graph
  {
  public:
    Graph() = default;

    template<typename TKey>
    Domain<TKey>& addDomain(std::string name)
    {
      auto domain = std::make_unique<Domain<TKey>>(name);
      Domain<TKey>& retVal = *domain;
      _domains.push_back(std::move(domain));
      return retVal;
    }

    bool isComputeRequired() const
    {
      return std::any_of(_domains.begin(), _domains.end(),
        [](const std::unique_ptr<DomainBase>& domain) { return domain->isComputeRequired(); });
    }

    bool isPublishRequired() const
    {
      return std::any_of(_domains.begin(), _domains.end(),
        [](const std::unique_ptr<DomainBase>& domain) { return domain->isPublishRequired(); });
    }

    void compute()
    {
      for (auto const& domain : _domains)
        domain->compute();
    }

    void publish()
    {
      for (auto const& domain : _domains)
        domain->publish();
    }

    std::vector<std::unique_ptr<DomainBase>>::iterator begin() { return _domains.begin(); }
    std::vector<std::unique_ptr<DomainBase>>::iterator end()   { return _domains.end(); }

    std::vector<std::unique_ptr<DomainBase>>::const_iterator begin() const { return _domains.begin(); }
    std::vector<std::unique_ptr<DomainBase>>::const_iterator end()   const { return _domains.end(); }

    void toDot(std::ostream& o) const;

    DomainBase* findDomain(std::string domainName) const
    {
      // TODO this could be O(1) instead of O(N) if we want to manage another map
      for (auto const& domain : _domains)
        if (domain->getName() == domainName)
          return domain.get();
      return nullptr;
    }

  private:
    Graph(const Graph&) = delete;
    Graph& operator=(const Graph&) = delete;

    std::vector<std::unique_ptr<DomainBase>> _domains;
  };
}

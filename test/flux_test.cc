#include <gtest/gtest.h>

#include <rapidjson/document.h>

#include "../flux/flux.hh"

#include <camshaft/uuid.hh>

using namespace flux;
using namespace rapidjson;
using namespace std;

// Notes
//
// - not thread safe
// - build a network of (regular) fields and computed fields
// - clients subscribe to all types of field
// - processes set data on fields
// - processes call 'compute' to produce computed (derived) field values
// - processes call 'publish' to push changes to any subscribed clients

// TODO do we need freeze?
// TODO think about time series
// TODO think about stateful computations such as moving average
// TODO think about timing elements such as conflation
// TODO do we need a timestamp on values?
// TODO remove field values (i.e. not just additive)

// TODO un-subscribe

TEST(FieldTest, setValueAndFindValue)
{
  Graph graph;
  auto& domain = graph.addDomain<int>("domain");
  auto& field = domain.createField<double>("field");

  EXPECT_EQ(field.end(), field.find(1));
  EXPECT_EQ(0, field.count());

  field.setValue(1, 0.1);

  auto it1 = field.find(1);
  auto it2 = field.find(2);

  ASSERT_NE(field.end(), it1);
  EXPECT_EQ(field.end(), it2);

  EXPECT_EQ(0.1, it1->second);

  any a = field.getValue(1);
  EXPECT_FALSE(a.empty());
  EXPECT_TRUE(a.is<double>());
  EXPECT_EQ(0.1, any_cast<double>(a));
}

TEST(FieldTest, setValueRepeatedly)
{
  Graph graph;
  auto& domain = graph.addDomain<int>("domain");
  auto& field = domain.createField<double>("field");

  field.setValue(1, 0.1);
  field.setValue(1, 0.2);
  field.setValue(1, 0.3);

  auto it1 = field.find(1);
  EXPECT_EQ(1, field.count());
  ASSERT_NE(field.end(), it1);
  EXPECT_EQ(0.3, it1->second);
}

TEST(FieldTest, observeField)
{
  Graph graph;

  auto& domain = graph.addDomain<int>("domain");
  auto& field = domain.createField<double>("field");

  int observedKey = 0;
  double observedVal = 0;
  int observerCallCount = 0;

  field.subscribe([&](int key, double val)
  {
    observedVal = val;
    observedKey = key;
    observerCallCount++;
  });

  auto key = 123;

  field.setValue(key, 1.1);

  // Value available immediately
  ASSERT_NE(field.end(), field.find(key));
  EXPECT_EQ(1.1, field.find(key)->second);
  EXPECT_EQ(0, observerCallCount);
  EXPECT_FALSE(graph.isComputeRequired());
  EXPECT_TRUE(graph.isPublishRequired());

  graph.compute();

  // In this case, compute does nothing
  ASSERT_NE(field.end(), field.find(key));
  EXPECT_EQ(1.1, field.find(key)->second);
  EXPECT_EQ(0, observerCallCount);
  EXPECT_FALSE(graph.isComputeRequired());
  EXPECT_TRUE(graph.isPublishRequired());

  graph.publish();

  EXPECT_EQ(1, observerCallCount);
  EXPECT_EQ(123, observedKey);
  EXPECT_EQ(1.1, observedVal);
  EXPECT_FALSE(graph.isComputeRequired());
  EXPECT_FALSE(graph.isPublishRequired());
}

TEST(ComputedFieldTest, observeComputedField)
{
  Graph graph;

  auto& domain = graph.addDomain<int>("domain");
  auto& field1 = domain.createField<double>("field1");
  auto& field2 = domain.createField<double>("field2");

  int computeCallCount = 0;

  auto& sum = domain.compute<double>(
    "computed",
    { &field1, &field2 },
    [&](const Params& vals)
    {
      computeCallCount++;
      return vals(field1) + vals(field2);
    }
  );

  int observedKey = 0;
  double observedVal = 0;
  int observerCallCount = 0;

  sum.subscribe([&](int key, double val)
  {
    observedVal = val;
    observedKey = key;
    observerCallCount++;
  });

  auto key = 123;

  field1.setValue(key, 1.1);
  field2.setValue(key, 2.2);

  EXPECT_EQ(sum.end(), sum.find(key));
  EXPECT_EQ(0, observerCallCount);
  EXPECT_TRUE(graph.isComputeRequired());
  EXPECT_FALSE(graph.isPublishRequired());

  graph.compute();

  EXPECT_EQ(0, observerCallCount);
  EXPECT_FALSE(graph.isComputeRequired());
  EXPECT_TRUE(graph.isPublishRequired());

  graph.publish();

  EXPECT_EQ(1, observerCallCount);
  EXPECT_EQ(1, computeCallCount);
  EXPECT_EQ(123, observedKey);
  EXPECT_DOUBLE_EQ(3.3, observedVal);
  EXPECT_FALSE(graph.isComputeRequired());
  EXPECT_FALSE(graph.isPublishRequired());

  auto it = sum.find(key);
  ASSERT_NE(sum.end(), it);
  EXPECT_DOUBLE_EQ(3.3, it->second);

  observerCallCount = 0;
  computeCallCount = 0;

  field1.setValue(key, 10.0);
  graph.compute();
  graph.publish();

  it = sum.find(key);
  ASSERT_NE(sum.end(), it);
  EXPECT_DOUBLE_EQ(10.0 + 2.2, it->second);
}

TEST(ComputedFieldTest, differentKeys)
{
  Graph graph;
  auto& domain = graph.addDomain<int>("domain");
  auto& field1 = domain.createField<double>("field1");
  auto& field2 = domain.createField<double>("field2");

  int computeCallCount = 0;

  domain.compute<double>(
    "computed",
    { &field1, &field2 },
    [&](const Params& vals)
    {
      computeCallCount++;
      return vals(field1) + vals(field2);
    });

  field1.setValue(123, 1.1); // different keys
  field2.setValue(321, 2.2); //

  EXPECT_EQ(0, computeCallCount);
  EXPECT_FALSE(graph.isComputeRequired());
  EXPECT_FALSE(graph.isPublishRequired());
}

TEST(ComputedFieldTest, getRelatedKey)
{
  Graph graph;
  auto& instrument = graph.addDomain<string>("instrument");
  auto& trade = graph.addDomain<Uuid>("trade");

  auto& tradeQaid = trade.createRelationTo(instrument);

  Uuid tradeId = Uuid::random();
  tradeQaid.setValue(tradeId, "QAID");

  EXPECT_EQ("QAID", any_cast<string>(trade.getRelatedKey(any(tradeId), instrument)));
}

TEST(ComputedFieldTest, computeAcrossRelation)
{
  // Set up the computation network

  Graph graph;

  //
  // Domains
  //

  // Domains map over a specific type of key to values by field

  auto& instrument = graph.addDomain<string>("instrument");
  auto& trade = graph.addDomain<Uuid>("trade");

  //
  // Fields
  //

  // Fields store the most recent value of a given type

  auto& lastPx = instrument.createField<double>("lastPx");
  auto& usdRate = instrument.createField<double>("usdRate");
  auto& adjHistClosePx = instrument.createField<double>("adjHistClosePx");
  auto& sodPos = instrument.createField<long>("sodPos");

  auto& cumQty = trade.createField<unsigned>("cumQty");
  auto& avgPx = trade.createField<double>("avgPx");

  // Relations between domains

  auto& tradeQaid = trade.createRelationTo(instrument);

//  wee.freeze();

  // Computed fields

  // TODO these computations need to take the side into account

  int tradeReturnComputeCount = 0;
  auto& tradeReturn = trade.compute<double>(
    "tradeReturn",
    { &cumQty, &lastPx, &avgPx, &usdRate },
    [&](const Params& vals)
    {
      tradeReturnComputeCount++;
      return vals(cumQty) * (vals(lastPx) - vals(avgPx)) * vals(usdRate);
    });

  int posReturnComputeCount = 0;
  auto& posReturn = instrument.compute<double>(
    "posReturn",
    { &sodPos, &lastPx, &adjHistClosePx, &usdRate },
    [&](const Params& vals)
    {
      posReturnComputeCount++;
      return vals(sodPos) * (vals(lastPx) - vals(adjHistClosePx)) * vals(usdRate);
    });

  (void)posReturn;
  (void)tradeReturn;

  Uuid tradeId = Uuid::random();
  string instrumentId = "QAID";

  lastPx.setValue(instrumentId, 101.0);
  usdRate.setValue(instrumentId, 2.0);
  adjHistClosePx.setValue(instrumentId, 100.0);
  sodPos.setValue(instrumentId, 50.0);

  cumQty.setValue(tradeId, 1000);
  avgPx.setValue(tradeId, 102.0);

  // TODO split this into two test classes, with setup functions

  // Dependencies should not be met yet, so calling 'compute' should have no effect
  graph.compute();
  EXPECT_EQ(0, tradeReturnComputeCount);
  EXPECT_EQ(1, posReturnComputeCount);
  EXPECT_FALSE(graph.isComputeRequired());

  tradeQaid.setValue(tradeId, instrumentId);

  EXPECT_TRUE(graph.isComputeRequired());
  EXPECT_EQ(0, tradeReturnComputeCount);
  EXPECT_EQ(1, posReturnComputeCount);

  graph.compute();

  EXPECT_FALSE(graph.isComputeRequired());
  EXPECT_EQ(1, tradeReturnComputeCount);
  EXPECT_EQ(1, posReturnComputeCount);
}

void testOrder(const vector<int>& order)
{
//  cout << "--------------------------------------------------------------------" << endl;
//  cout << "Testing order: " << order[0] << " " << order[1] << " " << order[2] << endl;

  Graph graph;
  auto& intDomain = graph.addDomain<int>("ints");
  auto& dblDomain = graph.addDomain<double>("doubles");

  auto& intValue = intDomain.createField<int>("int");

  auto& dblValue = dblDomain.createField<double>("floating-point");

  auto& roundedValue = dblDomain.createRelationTo(intDomain);

  int computeCount = 0;

  auto& computed = dblDomain.compute<double>(
    "computed",
    {&intValue, &roundedValue, &dblValue},
    [&](const Params& vals)
    {
      EXPECT_DOUBLE_EQ((double)vals(intValue), vals(dblValue));
      computeCount++;
      return (double)vals(intValue) + vals(dblValue);
    });

  EXPECT_FALSE(graph.isComputeRequired());

  for (int i : order)
  {
    if (i == 1)      dblValue.setValue(1.0, 1);
    else if (i == 2) intValue.setValue(1, 1);
    else if (i == 3) roundedValue.setValue(1.0, 1);
  }

  ASSERT_TRUE(graph.isComputeRequired());
  graph.compute();
  ASSERT_EQ(1, computeCount);
  ASSERT_DOUBLE_EQ(2.0, computed.getValue(1.0));

//  cout << "SUCCESSS!!!!!! " << computeCount << endl;
}

TEST(ComputedFieldTest, computeAcrossRelationOrderings)
{
  vector<int> order {1,2,3};

  std::sort(order.begin(), order.end());

  testOrder(order);
  while (std::next_permutation(order.begin(), order.end()))
    testOrder(order);
}

TEST(ComputedFieldTest, computeAcrossMultipleRelations)
{
  Graph graph;

  //
  // Domains
  //

  auto& instrument = graph.addDomain<string>("instrument");
  auto& trade = graph.addDomain<Uuid>("trade");
  auto& currency = graph.addDomain<string>("currency");

  //
  // Fields
  //

  auto& lastPx = instrument.createField<double>("lastPx");

  auto& cumQty = trade.createField<unsigned>("cumQty");
  auto& avgPx = trade.createField<double>("avgPx");

  auto& usdRate = currency.createField<double>("usdRate");

  // Relations between domains

  auto& tradeQaid = trade.createRelationTo(instrument);
  auto& tradeCcy = instrument.createRelationTo(currency);

  // Computed fields

  int tradeReturnComputeCount = 0;
  auto& tradeReturn = trade.compute<double>(
    "tradeReturn",
    { &cumQty, &lastPx, &avgPx, &usdRate },
    [&](const Params& vals)
    {
      tradeReturnComputeCount++;
      return vals(cumQty) * (vals(lastPx) - vals(avgPx)) * vals(usdRate);
    });

  (void)tradeReturn;

  Uuid tradeId = Uuid::random();
  string instrumentId = "@VOD";
  string ccy = "GBP";

  lastPx.setValue(instrumentId, 101.0);
  cumQty.setValue(tradeId, 1000);
  avgPx.setValue(tradeId, 102.0);
  usdRate.setValue(ccy, 2.0);
  tradeCcy.setValue(instrumentId, ccy);

  EXPECT_FALSE(graph.isComputeRequired());

  // Dependencies should not be met yet, so calling 'compute' should have no effect
  graph.compute();

  EXPECT_EQ(0, tradeReturnComputeCount);
  EXPECT_FALSE(graph.isComputeRequired());

  tradeQaid.setValue(tradeId, instrumentId);

  EXPECT_TRUE(graph.isComputeRequired());
  EXPECT_EQ(0, tradeReturnComputeCount);

  graph.compute();

  EXPECT_FALSE(graph.isComputeRequired());
  EXPECT_EQ(1, tradeReturnComputeCount);
}

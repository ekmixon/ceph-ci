// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "rgw_xml.h"
#include "rgw_inventory.h"
#include <gtest/gtest.h>
#include <numeric>
#include <string>
#include <vector>
#include <stdexcept>

namespace {
  rgw::inv::Configuration inventory1;
  rgw::inv::Configuration inventory1_1;
  rgw::inv::Configuration inventory1_2;
}

static const char* inv_xml_1 =
R"(<?xml version="1.0" encoding="UTF-8"?>
<InventoryConfiguration xmlns="http://s3.amazonaws.com/doc/2006-03-01/">
   <Id>report1</Id>
   <IsEnabled>true</IsEnabled>
   <Filter>
      <Prefix>filterPrefix</Prefix>
   </Filter>
   <Destination>
      <S3BucketDestination>
         <Format>CSV</Format>
         <AccountId>123456789012</AccountId>
         <Bucket>arn:aws:s3:::destination-bucket</Bucket>
         <Prefix>prefix1</Prefix>
         <Encryption>
            <SSE-KMS>
               <KeyId>arn:aws:kms:us-west-2:111122223333:key/1234abcd-12ab-34cd-56ef-1234567890ab</KeyId>
            </SSE-KMS>
         </Encryption>
      </S3BucketDestination>
   </Destination>
   <Schedule>
      <Frequency>Daily</Frequency>
   </Schedule>
   <IncludedObjectVersions>All</IncludedObjectVersions>
   <OptionalFields>
      <Field>Size</Field>
      <Field>LastModifiedDate</Field>
      <Field>ETag</Field>
      <Field>StorageClass</Field>
      <Field>IsMultipartUploaded</Field>
      <Field>ReplicationStatus</Field>
      <Field>EncryptionStatus</Field>
      <Field>ObjectLockRetainUntilDate</Field>
      <Field>ObjectLockMode</Field>
      <Field>ObjectLockLegalHoldStatus</Field>
   </OptionalFields>
</InventoryConfiguration>
)";

TEST(TestInventoryConfiguration, InvXML1)
{
  RGWXMLDecoder::XMLParser parser;
  ASSERT_TRUE(parser.init());
  ASSERT_TRUE(parser.parse(inv_xml_1, strlen(inv_xml_1), 1));
  auto result = RGWXMLDecoder::decode_xml("InventoryConfiguration", inventory1,
					  &parser, true);
  ASSERT_TRUE(result);
  // validate members
  ASSERT_EQ(inventory1.id, "report1");
  ASSERT_EQ(inventory1.filter.prefix, "filterPrefix");
  ASSERT_EQ(inventory1.destination.format, rgw::inv::Format::CSV);
  ASSERT_EQ(inventory1.destination.account_id, "123456789012");
  ASSERT_EQ(inventory1.destination.bucket_arn,
	    "arn:aws:s3:::destination-bucket");
  ASSERT_EQ(inventory1.destination.prefix, "prefix1");
  ASSERT_EQ(inventory1.destination.encryption.kms.key_id,
	    "arn:aws:kms:us-west-2:111122223333:key/1234abcd-12ab-34cd-56ef-1234567890ab");
  ASSERT_EQ(inventory1.schedule.frequency, rgw::inv::Frequency::Daily);
  ASSERT_EQ(inventory1.versions, rgw::inv::ObjectVersions::All);
  // check optional fields
  for (auto& field : rgw::inv::field_table) {
    // check the full sequence of defined field types, less FieldType::None and not-present
    // FieldType::IntelligentTieringAccessTier and FieldType::BucketKeyStatus
    if ((field.ord == rgw::inv::FieldType::None) ||
	(field.ord == rgw::inv::FieldType::BucketKeyStatus) ||
	(field.ord == rgw::inv::FieldType::IntelligentTieringAccessTier)) {
      continue;
    }
    ASSERT_TRUE(inventory1.optional_fields & rgw::inv::shift_field(field.ord));
  }
}

TEST(TestIdempotentParse, InvXML1)
{
  RGWXMLDecoder::XMLParser parser;
  ASSERT_TRUE(parser.init());
  ASSERT_TRUE(parser.parse(inv_xml_1, strlen(inv_xml_1), 1));
  auto result = RGWXMLDecoder::decode_xml("InventoryConfiguration", inventory1_1,
					  &parser, true);
  ASSERT_TRUE(result);
  // same doc serializes to same structure
  ASSERT_TRUE(inventory1 == inventory1_1);
}

TEST(TestIdempotentEncodeDecode, InvXML1)
{
  ceph::buffer::list bl1, bl2, bl3;
  inventory1.encode(bl1);
  inventory1_1.encode(bl2);
  // equivalent serialized forms compare equal
  ASSERT_EQ(bl1, bl2);
  // deserialized form compares equal with original
  rgw::inv::Configuration inventory1_3;
  decode(inventory1_3, bl2);
  ASSERT_EQ(inventory1, inventory1_3);
}

// Names and destinations of batch outputs (batch_destination.hpp): patterns, Windows name rules,
// long and Unicode names, mirrored subfolders; the batch's journal documents and receipt details.
// Pure functions; no files.

#include <filesystem>
#include <string>

#include <gtest/gtest.h>

#include <lucent/application/batch_destination.hpp>
#include <lucent/application/batch_service.hpp>

#include "batch_documents.hpp"

namespace lucent::app {
namespace {

using exporter::ExportFormat;

std::wstring expand(std::string_view pattern, const OutputNameFields& fields, std::uint32_t index = 1,
                    ExportFormat format = ExportFormat::Jpeg) {
    auto parsed = parse_name_pattern(pattern);
    EXPECT_TRUE(parsed) << (parsed ? std::string() : parsed.error().describe());
    if (!parsed) {
        return {};
    }
    auto name = expand_output_name(*parsed, fields, format, index);
    EXPECT_TRUE(name) << (name ? std::string() : name.error().describe());
    return name ? name->native() : std::wstring();
}

OutputNameFields fields_for(std::wstring source, std::uint32_t revision = 0) {
    OutputNameFields fields;
    fields.source = std::filesystem::path(L"C:\\Photos") / source;
    fields.revision_number = revision;
    return fields;
}

TEST(BatchNames, DefaultPatternUsesStemAndRevision) {
    EXPECT_EQ(expand(kDefaultNamePattern, fields_for(L"IMG_0001.CR3", 3)), L"IMG_0001_Lucent_3.jpg");
    EXPECT_EQ(expand(kDefaultNamePattern, fields_for(L"IMG_0001.CR3", 0)), L"IMG_0001_Lucent_0.jpg");
    EXPECT_EQ(expand(kDefaultNamePattern, fields_for(L"IMG_0001.CR3", 3), 2, ExportFormat::Tiff),
              L"IMG_0001_Lucent_3_2.tif");
    EXPECT_EQ(expand(kSingleExportNamePattern, fields_for(L"IMG_0001.CR3")), L"IMG_0001_Lucent.jpg");
}

TEST(BatchNames, SequenceAndImageTokens) {
    OutputNameFields fields = fields_for(L"pano.dng");
    fields.ordinal = 41;
    fields.subimage = 1;
    fields.subimage_count = 3;
    EXPECT_EQ(expand("Trip_{sequence}", fields), L"Trip_0042_img2.jpg");  // multi-image without {image}
    EXPECT_EQ(expand("{original-stem}-{image}of3", fields), L"pano-2of3.jpg");
    fields.ordinal = 123456;
    EXPECT_EQ(expand("{sequence}", fields), L"123457_img2.jpg");
    fields.subimage_count = 1;
    fields.subimage = 0;
    EXPECT_EQ(expand("{sequence}", fields), L"123457.jpg");
}

TEST(BatchNames, PatternsAreValidated) {
    for (const char* bad : {"", "{original-stem", "x}", "{unknown}", "Lucent_{revision}", "a/{original-stem}",
                            "{original-stem}\\b", "{original-stem}:x", "{original-stem}?", "{original-stem}*"}) {
        auto parsed = parse_name_pattern(bad);
        EXPECT_FALSE(parsed) << bad;
        if (!parsed) {
            EXPECT_EQ(parsed.error().code, ErrorCode::InvalidArgument) << bad;
        }
    }
    EXPECT_FALSE(parse_name_pattern(std::string(kMaxNamePatternBytes + 1, 'a') + "{sequence}"));
    EXPECT_FALSE(parse_name_pattern("\xff{sequence}"));  // not UTF-8
    EXPECT_TRUE(parse_name_pattern("{sequence}"));
    EXPECT_TRUE(parse_name_pattern("Ünïcödé {original-stem} 写真"));
}

TEST(BatchNames, WindowsRulesAreApplied) {
    // Trailing dots and spaces vanish on Windows; device names are reserved before the first dot.
    EXPECT_EQ(expand("{original-stem}. ", fields_for(L"a.dng")), L"a.jpg");
    EXPECT_EQ(expand("{original-stem}", fields_for(L"CON.dng")), L"_CON.jpg");
    EXPECT_EQ(expand("{original-stem}", fields_for(L"lpt1.nef")), L"_lpt1.jpg");
    EXPECT_EQ(expand("{original-stem}", fields_for(L"COM\u00B9.nef")), L"_COM\u00B9.jpg");
    EXPECT_EQ(expand("{original-stem}x", fields_for(L"CON.dng")), L"CONx.jpg");
    EXPECT_TRUE(is_reserved_device_name(L"conout$"));
    EXPECT_TRUE(is_reserved_device_name(L"aux.tar.gz"));
    EXPECT_FALSE(is_reserved_device_name(L"console"));

    EXPECT_TRUE(validate_file_name(L"photo.jpg"));
    EXPECT_FALSE(validate_file_name(L"pho<to.jpg"));
    EXPECT_FALSE(validate_file_name(L"photo.jpg."));
    EXPECT_FALSE(validate_file_name(L"NUL.jpg"));
    EXPECT_FALSE(validate_file_name(std::wstring(256, L'a')));
    EXPECT_FALSE(validate_file_name(std::wstring(L"bad\xD800name")));  // unpaired surrogate

    EXPECT_EQ(sanitize_file_name(L"a<b>c"), L"a_b_c");
    EXPECT_EQ(sanitize_file_name(L"..."), L"_");
    EXPECT_EQ(sanitize_file_name(std::wstring(L"x\xDC00y")), L"x\uFFFDy");
}

TEST(BatchNames, LongNamesAreShortenedWithoutSplittingSurrogatePairs) {
    // 300 characters of U+1F4F7 (camera emoji, a surrogate pair each).
    std::wstring stem;
    for (int i = 0; i < 150; ++i) {
        stem += L"\U0001F4F7";
    }
    const std::wstring name = expand(kDefaultNamePattern, fields_for(stem + L".dng", 12), 9999);
    EXPECT_LE(name.size(), kMaxFileNameUnits);
    EXPECT_TRUE(name.ends_with(L"_9999.jpg"));
    EXPECT_TRUE(validate_file_name(name)) << "the name must remain valid Unicode";
}

TEST(BatchNames, IndexLimits) {
    auto pattern = parse_name_pattern(kDefaultNamePattern);
    ASSERT_TRUE(pattern);
    EXPECT_FALSE(expand_output_name(*pattern, fields_for(L"a.dng"), ExportFormat::Jpeg, 0));
    auto too_far = expand_output_name(*pattern, fields_for(L"a.dng"), ExportFormat::Jpeg, kMaxOutputNameIndex + 1);
    ASSERT_FALSE(too_far);
    EXPECT_EQ(too_far.error().code, ErrorCode::LimitExceeded);
    OutputNameFields bad = fields_for(L"a.dng");
    bad.subimage = 2;
    EXPECT_FALSE(expand_output_name(*pattern, bad, ExportFormat::Jpeg, 1));
}

TEST(BatchDestinations, MirroredSubfolders) {
    const std::filesystem::path out = L"D:\\Exports";
    const std::filesystem::path root = L"C:\\Photos\\2026";
    EXPECT_EQ(output_directory_for(out, true, root, L"C:\\Photos\\2026\\Iceland\\Day 1\\a.cr3"),
              out / L"Iceland" / L"Day 1");
    EXPECT_EQ(output_directory_for(out, true, root, L"c:\\photos\\2026\\b.cr3"), out);  // case-insensitive root
    EXPECT_EQ(output_directory_for(out, true, root, L"C:\\Elsewhere\\c.cr3"), out);     // outside the root
    EXPECT_EQ(output_directory_for(out, false, root, L"C:\\Photos\\2026\\Iceland\\a.cr3"), out);
    // A mirrored component that Windows reserves is made valid.
    EXPECT_EQ(output_directory_for(out, true, root, L"C:\\Photos\\2026\\aux\\a.cr3"), out / L"_aux");
}

TEST(BatchDestinations, CommonParentFolder) {
    EXPECT_EQ(common_parent_folder({L"C:\\A\\B\\x.dng", L"C:\\A\\B\\C\\y.dng"}), std::filesystem::path(L"C:\\A\\B"));
    EXPECT_EQ(common_parent_folder({L"C:\\A\\B\\x.dng", L"c:\\a\\D\\y.dng"}), std::filesystem::path(L"C:\\A"));
    EXPECT_TRUE(common_parent_folder({L"C:\\A\\x.dng", L"D:\\A\\y.dng"}).empty());
    EXPECT_TRUE(common_parent_folder({}).empty());
}

TEST(BatchDestinations, CollisionPolicySpellings) {
    EXPECT_EQ(to_string(CollisionPolicy::UniqueName), "unique_name");
    EXPECT_EQ(to_string(CollisionPolicy::Overwrite), "overwrite");
    EXPECT_EQ(collision_policy_from_string("overwrite").value(), CollisionPolicy::Overwrite);
    EXPECT_FALSE(collision_policy_from_string("replace"));
}

TEST(BatchDestinations, PathKeysFoldCase) {
    EXPECT_EQ(folded_path_key(L"C:\\Out\\IMG_0001_Lucent.JPG"), folded_path_key(L"c:\\out\\img_0001_lucent.jpg"));
    EXPECT_NE(folded_path_key(L"C:\\Out\\a.jpg"), folded_path_key(L"C:\\Out\\b.jpg"));
}

// ---- Journal documents (batch_documents.hpp) and receipt details -------------------------------

TEST(BatchDocuments, RoundTripAndRejectNewerVersions) {
    detail::DestinationDocument destination;
    destination.folder = L"C:\\Exports\\Ünïcödé 写真";
    core::FileIdentity folder_object;
    folder_object.volume_serial = 0x1234ABCDull;
    folder_object.file_id[0] = 0x42;
    folder_object.file_id[15] = 0x99;
    destination.folder_identity = folder_object;
    destination.name_pattern = "{original-stem}_{sequence}";
    destination.mirror_subfolders = true;
    destination.mirror_root = L"D:\\Photos";
    destination.collisions = CollisionPolicy::Overwrite;
    destination.temp_tag = L"0a1b2c3d";
    destination.write_report_files = false;
    destination.storage = core::StorageKind::Removable;
    auto destination_json = detail::write_destination(destination);
    ASSERT_TRUE(destination_json) << destination_json.error().describe();
    auto destination_back = detail::read_destination(*destination_json);
    ASSERT_TRUE(destination_back) << destination_back.error().describe();
    EXPECT_EQ(destination_back->folder, destination.folder);
    ASSERT_TRUE(destination_back->folder_identity);
    EXPECT_EQ(destination_back->folder_identity->volume_serial, folder_object.volume_serial);
    EXPECT_EQ(destination_back->folder_identity->file_id, folder_object.file_id);
    EXPECT_EQ(destination_back->mirror_root, destination.mirror_root);
    EXPECT_EQ(destination_back->collisions, CollisionPolicy::Overwrite);
    EXPECT_EQ(destination_back->temp_tag, destination.temp_tag);
    EXPECT_FALSE(destination_back->write_report_files);
    EXPECT_EQ(destination_back->storage, core::StorageKind::Removable);

    // The export document is canonical: its SHA-256 is part of every job identity.
    detail::ExportDocument output;
    output.settings = exporter::ExportSettings::tiff16(exporter::OutputProfile::DisplayP3);
    output.settings.metadata.include_gps = true;
    output.artist = "Ann";
    auto export_json = detail::write_export_document(output);
    ASSERT_TRUE(export_json);
    EXPECT_EQ(export_json->find(' '), std::string::npos) << "no whitespace";
    EXPECT_LT(export_json->find("\"allow_unsupported\""), export_json->find("\"artist\"")) << "sorted members";
    auto export_back = detail::read_export_document(*export_json);
    ASSERT_TRUE(export_back);
    EXPECT_EQ(export_back->settings.format, exporter::ExportFormat::Tiff);
    EXPECT_EQ(export_back->settings.profile, exporter::OutputProfile::DisplayP3);
    EXPECT_TRUE(export_back->settings.metadata.include_gps);
    EXPECT_EQ(export_back->artist, "Ann");
    EXPECT_FALSE(export_back->copyright.has_value());
    auto rewritten = detail::write_export_document(*export_back);
    ASSERT_TRUE(rewritten);
    EXPECT_EQ(*rewritten, *export_json);

    // A newer version is VersionMismatch, another schema or a damaged member Corrupt.
    std::string newer = *destination_json;
    newer.replace(newer.find("\"version\":1"), 11, "\"version\":2");
    auto refused = detail::read_destination(newer);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, ErrorCode::VersionMismatch);
    auto wrong = detail::read_export_document(*destination_json);
    ASSERT_FALSE(wrong);
    EXPECT_EQ(wrong.error().code, ErrorCode::Corrupt);
    std::string damaged = *destination_json;
    damaged.replace(damaged.find("\"temp_tag\":\"0a1b2c3d\""), 21, "\"temp_tag\":\"../evil!\"");
    EXPECT_FALSE(detail::read_destination(damaged));
}

TEST(BatchDocuments, ReceiptDetailsRoundTrip) {
    ReceiptDetails details;
    details.revision = catalog::RevisionId{17};
    details.revision_number = 4;
    details.recipe_sha256[0] = 0xAB;
    details.export_settings_sha256[31] = 0xCD;
    details.settings = exporter::ExportSettings::jpeg_srgb_q95();
    details.width = 8192;
    details.height = 5464;
    details.chroma_subsampling = exporter::ChromaSubsampling::Yuv444;
    details.encoder = "libjpeg-turbo";
    details.metadata_notes = {"GPS removed"};
    details.interpretation = raw::InterpretationStatus::OptionalOperationsSkipped;
    details.warnings = {"a warning"};
    details.render_seconds = 1.25;
    details.attempt = 2;
    details.name_index = 3;
    details.replaced_existing = true;
    details.review = {"Saved as IMG_0001_Lucent_4_3.jpg because IMG_0001_Lucent_4.jpg already existed."};
    auto json = write_receipt_details(details);
    ASSERT_TRUE(json) << json.error().describe();
    auto back = read_receipt_details(*json);
    ASSERT_TRUE(back) << back.error().describe();
    EXPECT_EQ(back->revision, details.revision);
    EXPECT_EQ(back->revision_number, 4u);
    EXPECT_EQ(back->recipe_sha256, details.recipe_sha256);
    EXPECT_EQ(back->export_settings_sha256, details.export_settings_sha256);
    EXPECT_EQ(back->width, 8192u);
    EXPECT_EQ(back->chroma_subsampling, exporter::ChromaSubsampling::Yuv444);
    EXPECT_EQ(back->interpretation, raw::InterpretationStatus::OptionalOperationsSkipped);
    EXPECT_EQ(back->render_seconds, 1.25);
    EXPECT_EQ(back->name_index, 3u);
    EXPECT_TRUE(back->replaced_existing);
    EXPECT_EQ(back->review, details.review);
    std::string newer = *json;
    newer.replace(newer.find("\"version\":1"), 11, "\"version\":9");
    auto refused = read_receipt_details(newer);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, ErrorCode::VersionMismatch);
    EXPECT_EQ(refused.error().where, "app.batch.receipt");
    EXPECT_FALSE(read_receipt_details("{}"));
}

}  // namespace
}  // namespace lucent::app

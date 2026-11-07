/**
 * @module cg.extract
 *
 * @brief 纯抽取层：游标树进、普通记录出。
 *
 * @internal
 * cg.extract.model 持有记录类型与唯一的 unique_key 构建器；
 * cg.extract.visitor 走查单个翻译单元。
 */
export module cg.extract;

export import cg.extract.model;
export import cg.extract.visitor;
